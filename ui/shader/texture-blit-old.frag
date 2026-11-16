#version 120

uniform sampler2D image;
varying  vec2 ex_tex_coord;

void main(void) {
    gl_FragColor  = texture2D(image, ex_tex_coord);
}
