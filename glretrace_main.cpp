/**************************************************************************
 *
 * Copyright 2011 Jose Fonseca
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 **************************************************************************/


#include <string.h>

#include "image.hpp"
#include "retrace.hpp"
#include "glproc.hpp"
#include "glretrace.hpp"


namespace glretrace {

bool double_buffer = false;
bool insideGlBeginEnd = false;
Trace::Parser parser;
glws::WindowSystem *ws = NULL;
glws::Visual *visual = NULL;
glws::Drawable *drawable = NULL;
glws::Context *context = NULL;

int window_width = 256, window_height = 256;

unsigned frame = 0;
long long startTime = 0;
bool wait = false;

bool benchmark = false;
const char *compare_prefix = NULL;
const char *snapshot_prefix = NULL;

unsigned dump_state = ~0;

void
checkGlError(int callIdx) {
    if (benchmark || insideGlBeginEnd) {
        return;
    }

    GLenum error = glGetError();
    if (error == GL_NO_ERROR) {
        return;
    }

    if (callIdx >= 0) {
        std::cerr << callIdx << ": ";
    }

    std::cerr << "warning: glGetError() = ";
    switch (error) {
    case GL_INVALID_ENUM:
        std::cerr << "GL_INVALID_ENUM";
        break;
    case GL_INVALID_VALUE:
        std::cerr << "GL_INVALID_VALUE";
        break;
    case GL_INVALID_OPERATION:
        std::cerr << "GL_INVALID_OPERATION";
        break;
    case GL_STACK_OVERFLOW:
        std::cerr << "GL_STACK_OVERFLOW";
        break;
    case GL_STACK_UNDERFLOW:
        std::cerr << "GL_STACK_UNDERFLOW";
        break;
    case GL_OUT_OF_MEMORY:
        std::cerr << "GL_OUT_OF_MEMORY";
        break;
    case GL_INVALID_FRAMEBUFFER_OPERATION:
        std::cerr << "GL_INVALID_FRAMEBUFFER_OPERATION";
        break;
    case GL_TABLE_TOO_LARGE:
        std::cerr << "GL_TABLE_TOO_LARGE";
        break;
    default:
        std::cerr << error;
        break;
    }
    std::cerr << "\n";
}


static void snapshot(Image::Image &image) {
    GLint drawbuffer = double_buffer ? GL_BACK : GL_FRONT;
    GLint readbuffer = double_buffer ? GL_BACK : GL_FRONT;
    glGetIntegerv(GL_DRAW_BUFFER, &drawbuffer);
    glGetIntegerv(GL_READ_BUFFER, &readbuffer);
    glReadBuffer(drawbuffer);
    glReadPixels(0, 0, image.width, image.height, GL_RGBA, GL_UNSIGNED_BYTE, image.pixels);
    checkGlError();
    glReadBuffer(readbuffer);
}


void frame_complete(unsigned call_no) {
    ++frame;
    
    if (snapshot_prefix || compare_prefix) {
        Image::Image *ref = NULL;
        if (compare_prefix) {
            char filename[PATH_MAX];
            snprintf(filename, sizeof filename, "%s%010u.png", compare_prefix, call_no);
            ref = Image::readPNG(filename);
            if (!ref) {
                return;
            }
            if (retrace::verbosity >= 0)
                std::cout << "Read " << filename << "\n";
        }
        
        Image::Image src(window_width, window_height, true);
        snapshot(src);

        if (snapshot_prefix) {
            char filename[PATH_MAX];
            snprintf(filename, sizeof filename, "%s%010u.png", snapshot_prefix, call_no);
            if (src.writePNG(filename) && retrace::verbosity >= 0) {
                std::cout << "Wrote " << filename << "\n";
            }
        }

        if (ref) {
            std::cout << "Snapshot " << call_no << " average precision of " << src.compare(*ref) << " bits\n";
            delete ref;
        }
    }

}


static void display(void) {
    Trace::Call *call;

    while ((call = parser.parse_call())) {
        const std::string &name = call->name();
        bool skipCall = false;

        if (retrace::verbosity >= 1) {
            std::cout << *call;
            std::cout.flush();
        }

        if ((name[0] == 'w' && name[1] == 'g' && name[2] == 'l') ||
            (name[0] == 'g' && name[1] == 'l' && name[2] == 'X')) {
            // XXX: We ignore the majority of the OS-specific calls for now
            if (name == "glXSwapBuffers" ||
                name == "wglSwapBuffers") {
                frame_complete(call->no);
                if (double_buffer)
                    drawable->swapBuffers();
                else
                    glFlush();
            } else if (name == "glXMakeCurrent" ||
                       name == "wglMakeCurrent") {
                glFlush();
                if (!double_buffer) {
                    frame_complete(call->no);
                }
            }
            skipCall = true;
        }

        if (!skipCall) {
            retrace::retrace_call(*call);
        }

        if (!insideGlBeginEnd && call->no >= dump_state) {
            state_dump(std::cout);
            exit(0);
        }

        delete call;
    }

    // Reached the end of trace
    glFlush();

    long long endTime = OS::GetTime();
    float timeInterval = (endTime - startTime) * 1.0E-6;

    if (retrace::verbosity >= -1) { 
        std::cout << 
            "Rendered " << frame << " frames"
            " in " <<  timeInterval << " secs,"
            " average of " << (frame/timeInterval) << " fps\n";
    }

    if (wait) {
        while (ws->processEvents()) {}
    } else {
        exit(0);
    }
}


static void usage(void) {
    std::cout << 
        "Usage: glretrace [OPTION] TRACE\n"
        "Replay TRACE.\n"
        "\n"
        "  -b           benchmark (no glgeterror; no messages)\n"
        "  -c PREFIX    compare against snapshots\n"
        "  -db          use a double buffer visual\n"
        "  -s PREFIX    take snapshots\n"
        "  -v           verbose output\n"
        "  -D CALLNO    dump state at specific call no\n"
        "  -w           wait on final frame\n";
}

extern "C"
int main(int argc, char **argv)
{

    int i;
    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (arg[0] != '-') {
            break;
        }

        if (!strcmp(arg, "--")) {
            break;
        } else if (!strcmp(arg, "-b")) {
            benchmark = true;
            retrace::verbosity = -1;
        } else if (!strcmp(arg, "-c")) {
            compare_prefix = argv[++i];
        } else if (!strcmp(arg, "-D")) {
            dump_state = atoi(argv[++i]);
            retrace::verbosity = -2;
        } else if (!strcmp(arg, "-db")) {
            double_buffer = true;
        } else if (!strcmp(arg, "--help")) {
            usage();
            return 0;
        } else if (!strcmp(arg, "-s")) {
            snapshot_prefix = argv[++i];
        } else if (!strcmp(arg, "-v")) {
            ++retrace::verbosity;
        } else if (!strcmp(arg, "-w")) {
            wait = true;
        } else {
            std::cerr << "error: unknown option " << arg << "\n";
            usage();
            return 1;
        }
    }

    ws = glws::createNativeWindowSystem();
    visual = ws->createVisual(double_buffer);
    drawable = ws->createDrawable(visual);
    drawable->resize(window_width, window_height);
    context = ws->createContext(visual);
    ws->makeCurrent(drawable, context);

    for ( ; i < argc; ++i) {
        if (parser.open(argv[i])) {
            startTime = OS::GetTime();
            display();
            parser.close();
        }
    }

    return 0;
}

} /* namespace glretrace */
