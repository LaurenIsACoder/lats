shaders = [
  ['texture-blit', 'frag'],
  ['texture-blit', 'vert'],
  ['texture-blit-flip', 'vert'],
  ['texture-blit-old', 'frag'],
  ['texture-blit-old', 'vert'],
  ['texture-blit-flip-old', 'vert'],
]

foreach e : shaders
  output = '@0@-@1@.h'.format(e[0], e[1])
  genh += custom_target(output,
                output: output,
                capture: true,
                input: files('@0@.@1@'.format(e[0], e[1])),
                command: [shaderinclude, '@INPUT0@'])
endforeach
