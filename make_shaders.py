#!/usr/bin/python

# Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import re
import string

header = """
// Auto generated DO NOT edit.

#ifndef WINDOW_MANAGER_GLES_SHADERS_H_
#define WINDOW_MANAGER_GLES_SHADERS_H_

#include <GLES2/gl2.h>

#include "base/basictypes.h"
#include "window_manager/compositor/gles/shader_base.h"

namespace window_manager {

$Headers

}  // namespace window_manager

#endif  // WINDOW_MANAGER_GLES_SHADERS_H_
"""

implementation = """
// Auto generated DO NOT edit.

#include "window_manager/compositor/gles/shaders.h"

#include <GLES2/gl2.h>

#include "base/logging.h"

#include "window_manager/compositor/gles/shader_base.h"

namespace window_manager {

$Sources

$Implementations

}  // namespace window_manager
"""

shader_template = """
class $ShaderName : public Shader {
 public:
  $ShaderName();

$Accessors

 private:
$Slots

  DISALLOW_COPY_AND_ASSIGN($ShaderName);
};
"""


ctor_template = """
$ShaderName::$ShaderName()
    : Shader($VertexSource, $FragmentSource) {
$FetchSlots
$UsedVertexAttribs
}
"""


shaders = {'TexColorVertex': 'compositor/gles/tex_color.glslv',
           'TexColorFragment': 'compositor/gles/tex_color.glslf',
           'TexShadeVertex': 'compositor/gles/tex_shade.glslv',
           'TexShadeFragment': 'compositor/gles/tex_shade.glslf',
           'NoAlphaColorFragment': 'compositor/gles/noalpha_color.glslf',
           'NoAlphaShadeFragment': 'compositor/gles/noalpha_shade.glslf'}
programs = [('TexColorShader', 'TexColorVertex', 'TexColorFragment'),
            ('TexShadeShader', 'TexShadeVertex', 'TexShadeFragment'),
            ('NoAlphaColorShader', 'TexColorVertex', 'NoAlphaColorFragment'),
            ('NoAlphaShadeShader', 'TexShadeVertex', 'NoAlphaShadeFragment')]


def CamelCase(identifier):
  return ''.join(s.title() for s in identifier.split('_'))


class Shader(object):
  def __init__(self, name, filename):
    self.name = name
    self.source = []
    self._Parse(filename)

  def _Parse(self, filename):
    slot_re = re.compile(r'^\s*(uniform|attribute)\s+(?:\S+\s+)*(\S+)\s*;')

    self.slots = []
    for line in file(filename):
      line = line.rstrip('\n')
      self.source.append(line)
      match = slot_re.match(line)
      if match:
        self.slots.append({'type': match.group(1), 'name': match.group(2)})

  def SourceName(self):
    return 'k' + self.name + 'Src'

  def QuotedSource(self):
    out = []
    out.append('static const char* %s =' % self.SourceName())
    for line in self.source:
      out.append(r'    "%s\n"' % line)
    out[-1] += ';'
    return '\n'.join(out)


class Program(object):
  def __init__(self, name, vertex, fragment):
    self.name = name
    self.vertex = vertex
    self.fragment = fragment
    self._MakeSlots()
    self.subs = {'Accessors': self.Accessors(),
                 'FetchSlots': self.FetchSlots(),
                 'FragmentSource': self.fragment.SourceName(),
                 'ShaderName': self.name,
                 'Slots': self.Slots(),
                 'UsedVertexAttribs': self.UsedVertexAttribs(),
                 'VertexSource': self.vertex.SourceName()}

  def _MakeSlots(self):
    all_slots = list(self.vertex.slots)
    all_slots.extend(self.fragment.slots)
    names = set(slot['name'] for slot in all_slots)

    self.slots = {}
    for slot in all_slots:
      assert slot['name'] not in self.slots
      self.slots[slot['name']] = slot['type']

  def FetchSlots(self):
    accessors = {'uniform': 'Uniform', 'attribute': 'Attrib'}
    out = []
    for slot in sorted(self.slots):
      out.append('  %s_ = glGet%sLocation(program(), "%s");' %
                 (slot, accessors[self.slots[slot]], slot))
      out.append('  CHECK(%s_ >= 0);' % slot)
    return '\n'.join(out)

  def Slots(self):
    out = []
    for slot in sorted(self.slots):
      out.append('  GLint %s_;' % slot)
    return '\n'.join(out)

  def UsedVertexAttribs(self):
    attribs = []
    for slot in sorted(self.slots):
      if self.slots[slot] == 'attribute':
        attribs.append(slot)

    prefix = '  SetUsedVertexAttribs('
    if len(attribs) == 1:
      out = '1 << %s_' % attribs[0]
    else:
      separator = ' |\n' + len(prefix) * ' '
      out = separator.join('(1 << %s_)' % attrib for attrib in attribs)
    return prefix + out + ');'

  def Accessors(self):
    out = []
    for slot in sorted(self.slots):
      out.append('  GLint %sLocation() const { return %s_; }' %
                 (CamelCase(slot), slot))
    return '\n'.join(out)

  def Header(self):
    template = string.Template(shader_template.lstrip('\n'))
    return template.substitute(**self.subs)

  def Implementation(self):
    template = string.Template(ctor_template.lstrip('\n'))
    return template.substitute(**self.subs)


def AddBuildRules(env):
  import SCons.Script

  def MakeShadersEmitter(target, source, env):
    source.append('make_shaders.py')
    for shader in shaders.itervalues():
      source.append(shader)
    target = ['compositor/gles/shaders.h', 'compositor/gles/shaders.cc']
    return target, source

  bld = SCons.Script.Builder(action='./make_shaders.py',
                             emitter=MakeShadersEmitter)
  env.Append(BUILDERS={'MakeShaders': bld})
  env.MakeShaders()


def main():
  shader_objects = {}
  for name, filename in shaders.items():
    shader_objects[name] = Shader(name, filename)

  program_objects = []
  for name, vertex, fragment in programs:
    program_objects.append(Program(name,
                                   shader_objects[vertex],
                                   shader_objects[fragment]))

  subs = {}
  subs['Headers'] = '\n\n'.join(program.Header()
                                for program in program_objects)
  subs['Implementations'] = '\n\n'.join(program.Implementation()
                                        for program in program_objects)
  subs['Sources'] = '\n\n'.join(shader.QuotedSource()
                                for shader in shader_objects.itervalues())

  header_file = file('compositor/gles/shaders.h', 'w')
  template = string.Template(header.lstrip('\n'))
  header_file.write(template.substitute(**subs))
  header_file.close()

  implementation_file = file('compositor/gles/shaders.cc', 'w')
  template = string.Template(implementation.lstrip('\n'))
  implementation_file.write(template.substitute(**subs))
  implementation_file.close()


if __name__ == '__main__':
  main()
