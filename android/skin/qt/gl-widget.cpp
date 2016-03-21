// Copyright 2016 The Android Open Source Project
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/skin/qt/gl-widget.h"

#include "android/base/memory/LazyInstance.h"

#include "GLES2/gl2.h"

#include <QResizeEvent>
#include <QtGlobal>

// Note that this header must come after ALL Qt headers.
// Including any Qt headers after it will cause compilation
// to fail spectacularly. This is because EGL indirectly
// includes X11 headers which define stuff that conflicts
// with Qt's own macros. It is a known issue.
#include "OpenGLESDispatch/EGLDispatch.h"
#include "OpenGLESDispatch/GLESv2Dispatch.h"

namespace {

// Helper class to hold a global GLESv2Dispatch that is initialized lazily
// in a thread-safe way. The instance is leaked on program exit.
struct MyGLESv2Dispatch : public GLESv2Dispatch {
    // Return pointer to global GLESv2Dispatch instance, or nullptr if there
    // was an error when trying to initialize/load the library.
    static const GLESv2Dispatch* get();

    MyGLESv2Dispatch() { mValid = gles2_dispatch_init(&mDispatch); }

private:
    GLESv2Dispatch mDispatch;
    bool mValid;
};

// Must be declared outside of MyGLESv2Dispatch scope due to the use of
// sizeof(T) within the template definition.
android::base::LazyInstance<MyGLESv2Dispatch> sGLESv2Dispatch =
        LAZY_INSTANCE_INIT;

// static
const GLESv2Dispatch* MyGLESv2Dispatch::get() {
    MyGLESv2Dispatch* instance = sGLESv2Dispatch.ptr();
    if (instance->mValid) {
        return &instance->mDispatch;
    } else {
        return nullptr;
    }
}

// Helper class used to laziy initialize the global EGL dispatch table
// in a thread safe way. Note that the dispatch table is provided by
// libOpenGLESDispatch as the 's_egl' global variable.
struct MyEGLDispatch : public EGLDispatch {
    // Return pointer to EGLDispatch table, or nullptr if there was
    // an error when trying to initialize/load the library.
    static const EGLDispatch* get();

    MyEGLDispatch() { mValid = init_egl_dispatch(); }

private:
    bool mValid;
};

android::base::LazyInstance<MyEGLDispatch> sEGLDispatch = LAZY_INSTANCE_INIT;

// static
const EGLDispatch* MyEGLDispatch::get() {
    MyEGLDispatch* instance = sEGLDispatch.ptr();
    if (instance->mValid) {
        return &s_egl;
    } else {
        return nullptr;
    }
}

}  // namespace

struct EGLState {
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
};

GLWidget::GLWidget(QWidget* parent) :
        QWidget(parent),
        mEGL(MyEGLDispatch::get()),
        mGLES2(MyGLESv2Dispatch::get()),
        mEGLState(nullptr),
        mValid(false),
        mEnableAA(false) {
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
}

bool GLWidget::ensureInit() {
    // If an error occured when loading the EGL/GLESv2 libraries, return false.
    if (!mEGL || !mGLES2) {
        return false;
    }

    // If already initialized, return mValid to indicate if an error occured.
    if (mEGLState) {
        return mValid;
    }

    mEGLState = new EGLState();
    mValid = false;

    mEGLState->display = mEGL->eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (mEGLState->display == EGL_NO_DISPLAY) {
        qWarning("Failed to get EGL display: EGL error %d",
                 mEGL->eglGetError());
        return false;
    }

    EGLint egl_maj, egl_min;
    EGLConfig egl_config;

    // Try to initialize EGL display.
    // Initializing an already-initialized display is OK.
    if (mEGL->eglInitialize(mEGLState->display, &egl_maj, &egl_min) ==
        EGL_FALSE) {
        qWarning("Failed to initialize EGL display: EGL error %d",
                 mEGL->eglGetError());
        return false;
    }

    // Get an EGL config.
    const EGLint config_attribs[] = {EGL_SURFACE_TYPE,
                                     EGL_WINDOW_BIT,
                                     EGL_RENDERABLE_TYPE,
                                     EGL_OPENGL_ES2_BIT,
                                     EGL_RED_SIZE,
                                     8,
                                     EGL_GREEN_SIZE,
                                     8,
                                     EGL_BLUE_SIZE,
                                     8,
                                     EGL_DEPTH_SIZE,
                                     24,
                                     EGL_SAMPLES,
                                     0,  // No multisampling
                                     EGL_NONE};
    EGLint num_config;
    EGLBoolean choose_result = mEGL->eglChooseConfig(
            mEGLState->display, config_attribs, &egl_config, 1, &num_config);
    if (choose_result == EGL_FALSE || num_config < 1) {
        qWarning("Failed to choose EGL config: EGL error %d",
                 mEGL->eglGetError());
        return false;
    }

    // Create a context.
    EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    mEGLState->context = mEGL->eglCreateContext(
            mEGLState->display, egl_config, EGL_NO_CONTEXT, context_attribs);
    if (mEGLState->context == EGL_NO_CONTEXT) {
        qWarning("Failed to create EGL context %d", mEGL->eglGetError());
    }

    // Finally, create a window surface associated with this widget.
    mEGLState->surface = mEGL->eglCreateWindowSurface(
            mEGLState->display, egl_config, (EGLNativeWindowType)(winId()),
            nullptr);
    if (mEGLState->surface == EGL_NO_SURFACE) {
        qWarning("Failed to create an EGL surface %d", mEGL->eglGetError());
        return false;
    }

    mValid = true;

    makeContextCurrent();
    mCanvas.reset(new GLCanvas(realPixelsWidth(), realPixelsHeight(), mGLES2));
    mAntiAliasing.reset(new GLAntiAliasing(mGLES2));
    initGL();

    return true;
}

void GLWidget::makeContextCurrent() {
    mEGL->eglMakeCurrent(mEGLState->display, mEGLState->surface,
                         mEGLState->surface, mEGLState->context);
}

void GLWidget::renderFrame() {
    if (!ensureInit()) {
        return;
    }
    makeContextCurrent();

    // Render 3D scene to texture.
    if (mEnableAA) {
        mCanvas->bind();
        repaintGL();
        mCanvas->unbind();
    } else {
        mGLES2->glViewport(0, 0, realPixelsWidth(), realPixelsHeight());
        repaintGL();
    }

    if (mEnableAA) {
        mAntiAliasing->apply(mCanvas->texture(),
                             realPixelsWidth(),
                             realPixelsHeight());
    }

    mEGL->eglSwapBuffers(mEGLState->display, mEGLState->surface);
}

void GLWidget::paintEvent(QPaintEvent*) {
    renderFrame();
}

void GLWidget::showEvent(QShowEvent*) {
    renderFrame();
}

void GLWidget::resizeEvent(QResizeEvent* e) {
    if (mEGLState) {
        // We should only call resizeGL and repaint if all the setup
        // has been done.
        // We should NOT attempt to initialize EGL state during a resize
        // event due to some subtleties on OS X. If we attempt to initialize
        // EGL state at the time the resize event is generated, the default
        // framebuffer will not be created.
        makeContextCurrent();
        // Re-create the framebuffer with new size.
        mCanvas.reset(new GLCanvas(e->size().width() * devicePixelRatio(),
                                   e->size().height() * devicePixelRatio(),
                                   mGLES2));
        resizeGL(e->size().width() * devicePixelRatio(),
                 e->size().height() * devicePixelRatio());
        renderFrame();
    }
}

GLWidget::~GLWidget() {
    if (mEGL && mEGLState) {
        mEGL->eglDestroyContext(mEGLState->display, mEGLState->context);
        mEGL->eglDestroySurface(mEGLState->display, mEGLState->surface);
        delete mEGLState;
    }
}