#ifndef Magnum_Trade_DdsImporter_h
#define Magnum_Trade_DdsImporter_h
/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015
              Vladimír Vondruš <mosra@centrum.cz>
    Copyright © 2015 Jonathan Hale <squareys@googlemail.com>

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

/** @file
 * @brief Class @ref Magnum::Trade::DdsImporter
 */

#include <Magnum/Trade/AbstractImporter.h>

namespace Magnum { namespace Trade {

/**
@brief DDS image importer plugin

Supports the following formats:
-   DDS uncompressed RGB, RGBA, BGR, BGRA, grayscale
-   DDS compressed DXT1, DXT3, DXT5

This plugin is built if `WITH_DDSIMPORTER` is enabled when building
Magnum Plugins. To use dynamic plugin, you need to load `DdsImporter`
plugin from `MAGNUM_PLUGINS_IMPORTER_DIR`. To use static plugin, you need to
request `DdsImporter` component of `MagnumPlugins` package in CMake and
link to `${MAGNUMPLUGINS_DDSIMPORTER_LIBRARIES}`. To use this as a
dependency of another plugin, you additionally need to add
`${MAGNUMPLUGINS_DDSIMPORTER_INCLUDE_DIRS}` to include path.

See @ref building-plugins, @ref cmake-plugins and @ref plugins for more
information.

The images are imported with @ref PixelType::UnsignedByte type and
@ref PixelFormat::RGB, @ref PixelFormat::RGBA, @ref PixelFormat::Red for
grayscale. BGR and BGRA images are converted to @ref PixelFormat::RGB,
@ref PixelFormat::RGBA respectively. If the image is compressed, they are
imported with @ref CompressedPixelFormat::RGBAS3tcDxt1,
@ref CompressedPixelFormat::RGBAS3tcDxt3 and @ref CompressedPixelFormat::RGBAS3tcDxt5.

In In OpenGL ES 2.0 grayscale images use @ref PixelFormat::Luminance instead
of @ref PixelFormat::Red.

Note: Mipmaps are currently imported under separate image data ids. You may
access them via @ref image2D(UnsignedInt)/@ref image3D(UnsignedInt) which will
return the n-th mip, a bigger n indicating a smaller mip.
*/
class DdsImporter: public AbstractImporter {
    public:
        /** @brief Default constructor */
        explicit DdsImporter();

        /** @brief Plugin manager constructor */
        explicit DdsImporter(PluginManager::AbstractManager& manager, std::string plugin);

        ~DdsImporter();

    private:
        Features doFeatures() const override;
        bool doIsOpened() const override;
        void doClose() override;
        void doOpenData(Containers::ArrayView<const char> data) override;

        UnsignedInt doImage2DCount() const override;
        std::optional<ImageData2D> doImage2D(UnsignedInt id) override;

        UnsignedInt doImage3DCount() const override;
        std::optional<ImageData3D> doImage3D(UnsignedInt id) override;

    private:
        struct File;

        std::unique_ptr<File> _f;
};

}}

#endif
