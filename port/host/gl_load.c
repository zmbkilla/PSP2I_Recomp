/* OpenGL entry points, loaded at run time. See gl_load.h. */
#include "gl_load.h"

#include <stdio.h>
#include <stdlib.h>

#define GL_DEFINE(ret, name, args) PFN_##name p_##name;
GL_FUNCS(GL_DEFINE)
#undef GL_DEFINE

int gl_load(void *(*get)(const char *name)) {
    int missing = 0;
#define GL_GET(ret, name, args) \
    p_##name = (PFN_##name)get(#name); \
    if (!p_##name) { fprintf(stderr, "gl: missing %s\n", #name); missing++; }
    GL_FUNCS(GL_GET)
#undef GL_GET
    return missing;
}

static GLuint shader(const char *what, GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, (GLsizei)sizeof log, NULL, log);
        fprintf(stderr, "gl: %s %s shader: %s\n", what, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint gl_program(const char *what, const char *vs, const char *fs) {
    GLuint v = shader(what, GL_VERTEX_SHADER, vs), f = shader(what, GL_FRAGMENT_SHADER, fs);
    if (!v || !f) { if (v) glDeleteShader(v); if (f) glDeleteShader(f); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    /* Fixed attribute and output slots, so no layout qualifiers are needed. */
    glBindAttribLocation(p, 0, "a_p");
    glBindAttribLocation(p, 1, "a_uv");
    glBindAttribLocation(p, 2, "a_c");
    glBindFragDataLocation(p, 0, "o_c");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, (GLsizei)sizeof log, NULL, log);
        fprintf(stderr, "gl: %s program: %s\n", what, log);
        return 0;
    }
    return p;
}
