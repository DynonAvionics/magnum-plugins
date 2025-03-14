#!/usr/bin/env python3

#
#   This file is part of Magnum.
#
#   Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
#               2020, 2021, 2022, 2023, 2024, 2025
#             Vladimír Vondruš <mosra@centrum.cz>
#   Copyright © 2021 Pablo Escobar <mail@rvrs.in>
#
#   Permission is hereby granted, free of charge, to any person obtaining a
#   copy of this software and associated documentation files (the "Software"),
#   to deal in the Software without restriction, including without limitation
#   the rights to use, copy, modify, merge, publish, distribute, sublicense,
#   and/or sell copies of the Software, and to permit persons to whom the
#   Software is furnished to do so, subject to the following conditions:
#
#   The above copyright notice and this permission notice shall be included
#   in all copies or substantial portions of the Software.
#
#   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
#   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
#   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
#   DEALINGS IN THE SOFTWARE.
#

import argparse
from collections import namedtuple
import itertools
import os
import re

parser = argparse.ArgumentParser()
parser.add_argument('magnum_source')
args = parser.parse_args()

magnum_dir = args.magnum_source
vulkan_header = os.path.join(magnum_dir, 'src/MagnumExternal/Vulkan/flextVk.h')

vulkan_formats = {}

with open(vulkan_header, encoding='utf-8') as f:
    lines = f.readlines()
    for line in lines:
        # Get numeric VkFormat values so we can dereference them directly
        # This also finds VK_FORMAT_FEATURE_* but that's no big deal since
        # there are no formats that start with FEATURE_
        match = re.search('^\s+VK_FORMAT_(\w+) = (\d+),?$', line)
        if match:
            assert(not match.group(1) in vulkan_formats)
            vulkan_formats[match.group(1)] = match.group(2)

Format = namedtuple('Format', 'compressed magnum vulkan_name vulkan suffix')
formats = []

format_header = os.path.join(magnum_dir, 'src/Magnum/Vk/PixelFormat.h')

with open(format_header, encoding='utf-8') as f:
    lines = f.readlines()
    for line in lines:
        # Get mapping from VkFormat to Magnum::Vk::PixelFormat
        # PixelFormat and Vk::PixelFormat names are identical
        match = re.search('^\s+(Compressed)?(\w+) = VK_FORMAT_(\w+),?$', line)
        if match:
            compressed = match.group(1) != None
            magnum_name = match.group(2)
            vulkan_name = match.group(3)
            assert(vulkan_name in vulkan_formats)

            suffix = re.search('\w+_([U|S](NORM|INT|FLOAT|RGB))\w*', vulkan_name)
            assert suffix != None
            assert suffix.group(1) != 'URGB'

            formats.append(Format(compressed, magnum_name, vulkan_name, vulkan_formats[vulkan_name], suffix.group(1)))

if len(formats) != 135:
    print('Unexpected number of formats')

# https://docs.python.org/dev/library/itertools.html#itertools-recipes
def partition(pred, iterable):
    t1, t2 = itertools.tee(iterable)
    return itertools.filterfalse(pred, t1), filter(pred, t2)

compressed = lambda f : f.compressed
formats, compressed_formats = partition(compressed, formats)

# There's no PVRTC2 in CompressedPixelFormat
compressed_formats = [f for f in compressed_formats if not f.magnum.startswith('Pvrtc2')]

header = '''/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
                2020, 2021, 2022, 2023, 2024, 2025
              Vladimír Vondruš <mosra@centrum.cz>
    Copyright © 2021 Pablo Escobar <mail@rvrs.in>

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

/* Autogenerated from formatMapping.py! Do not edit! */

'''

with open('formatMapping.hpp', 'w', encoding='utf-8') as outfile:
    outfile.write(header)
    outfile.write('/* VkFormat, PixelFormat, Implementation::VkFormatSuffix */\n')
    outfile.write('#ifdef _c\n')
    for format in formats:
        outfile.write('_c({}, {}, {}) /* VK_FORMAT_{} */\n'.format(format.vulkan , format.magnum, format.suffix, format.vulkan_name))
    outfile.write('#endif\n')

with open('compressedFormatMapping.hpp', 'w', encoding='utf-8') as outfile:
    outfile.write(header)
    outfile.write('/* VkFormat, CompressedPixelFormat, Implementation::VkFormatSuffix */\n')
    outfile.write('#ifdef _c\n')
    for format in compressed_formats:
        outfile.write('_c({}, {}, {}) /* VK_FORMAT_{} */\n'.format(format.vulkan , format.magnum, format.suffix, format.vulkan_name))
    outfile.write('#endif\n')
