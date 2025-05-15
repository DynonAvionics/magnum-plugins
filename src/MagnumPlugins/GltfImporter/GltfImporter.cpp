/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
                2020, 2021, 2022, 2023, 2024, 2025
              Vladimír Vondruš <mosra@centrum.cz>
    Copyright © 2021 Pablo Escobar <mail@rvrs.in>
    Copyright © 2022 Hugo Amiard <hugo.amiard@wonderlandengine.com>
    Copyright © 2023 Noeri Huisman <mrxz@users.noreply.github.com>

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

#include "GltfImporter.h"

#include <algorithm> /* std::sort() */
#include <cctype>
#include <unordered_map>
#include <Corrade/Containers/Array.h>
#include <Corrade/Containers/ArrayTuple.h>
#include <Corrade/Containers/ArrayView.h>
#include <Corrade/Containers/GrowableArray.h>
#include <Corrade/Containers/Optional.h>
#include <Corrade/Containers/Pair.h>
#include <Corrade/Containers/Reference.h>
#include <Corrade/Containers/StaticArray.h>
#include <Corrade/Containers/StridedBitArrayView.h>
#include <Corrade/Containers/String.h>
#include <Corrade/Containers/StringIterable.h>
#include <Corrade/Containers/StringStlHash.h>
#include <Corrade/Containers/StringView.h>
#include <Corrade/Containers/Triple.h>
#include <Corrade/Utility/Algorithms.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/Format.h>
#include <Corrade/Utility/Json.h>
#include <Corrade/Utility/MurmurHash2.h>
#include <Corrade/Utility/Path.h>
#include <Magnum/FileCallback.h>
#include <Magnum/Mesh.h>
#include <Magnum/PixelFormat.h>
#include <Magnum/Math/CubicHermite.h>
#include <Magnum/Math/FunctionsBatch.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Matrix4.h>
#include <Magnum/Math/Quaternion.h>
#include <Magnum/Trade/AnimationData.h>
#include <Magnum/Trade/CameraData.h>
#include <Magnum/Trade/ImageData.h>
#include <Magnum/Trade/LightData.h>
#include <Magnum/Trade/MaterialData.h>
#include <Magnum/Trade/MeshData.h>
#include <Magnum/Trade/SceneData.h>
#include <Magnum/Trade/SkinData.h>
#include <Magnum/Trade/TextureData.h>
#include <MagnumPlugins/AnyImageImporter/AnyImageImporter.h>

#include "MagnumPlugins/GltfImporter/decode.h"
#include "MagnumPlugins/GltfImporter/Gltf.h"

/* Otherwise std::unique() fails to compile on MSVC 2015 and libc++ 15 (commit
   https://github.com/llvm/llvm-project/commit/c9905b8cb0139f410ce63081989a328559e11374) */
#if defined(CORRADE_MSVC2015_COMPATIBILITY) || (defined(CORRADE_TARGET_LIBCXX) && _LIBCPP_VERSION >= 15)
#include <Corrade/Containers/StridedArrayViewStl.h>
#endif

/* We'd have to endian-flip everything that comes from buffers, plus the binary
   glTF headers, etc. Too much work, hard to automatically test because the
   HW is hard to get. */
#ifdef CORRADE_TARGET_BIG_ENDIAN
#error this code will not work on Big Endian, sorry
#endif

namespace Magnum { namespace Trade {

using namespace Containers::Literals;
using namespace Math::Literals;

namespace {

/* Data URI according to RFC 2397, used by loadUri() and
   setupOrReuseImporterForImage() */
inline bool isDataUri(const Containers::StringView uri) {
    return uri.hasPrefix("data:"_s);
}

/* Used by doOpenData() and doMesh() */
bool isBuiltinNumberedMeshAttribute(const Containers::StringView name) {
    const Containers::Array3<Containers::StringView> attributeNameNumber = name.partition('_');
    return
        (attributeNameNumber[0] == "TEXCOORD"_s ||
         attributeNameNumber[0] == "COLOR"_s ||
         /* Not a builtin MeshAttribute yet, but expected to be used by
            people until builtin support is added */
         attributeNameNumber[0] == "JOINTS"_s ||
         attributeNameNumber[0] == "WEIGHTS"_s) &&
        /* Assumes just a single number. glTF doesn't say anything about the
           upper limit, but for now it should be fine to allow 10 attributes
           at most. Thus TEXCOORD, TEXCOORD_SECOND or TEXCOORD_10 would fail
           this check. */
        /** @todo a more flexible parsing once we have our number parsers
            that don't rely on null-terminated strings */
        attributeNameNumber[2].size() == 1 && attributeNameNumber[2][0] >= '0' && attributeNameNumber[2][0] <= '9';
}

/* Used by doOpenData() */
bool isBuiltinMeshAttribute(Utility::ConfigurationGroup& configuration, const Containers::StringView name) {
    return
        name == "POSITION"_s ||
        name == "NORMAL"_s ||
        name == "TANGENT"_s ||
        name == "COLOR"_s ||
        name == configuration.value<Containers::StringView>("objectIdAttribute") ||
        isBuiltinNumberedMeshAttribute(name);
}

}

struct GltfImporter::BufferView {
    Containers::ArrayView<const char> data;
    UnsignedInt stride; /* 0 if not strided */
    UnsignedInt buffer; /* points into _d->buffers */
};

struct GltfImporter::Accessor {
    /* As the type is known, it's always a 2D view with layout as expected */
    Containers::StridedArrayView2D<const char> data;
    VertexFormat format;
    /* Points into _d->bufferViews. Can be ~UnsignedInt{}, which indicates that
       the accessor has no backing view or is sparse. The `data` then is a null
       view with just stride and size set to denote that it's meant to become a
       contiguous zero-initialized array of given element count and type
       size. */
    UnsignedInt bufferView;
    /* If non-empty, the accessor is sparse. Second dimension is index type
       size. */
    Containers::StridedArrayView2D<const char> sparseIndices;
    /* If non-empty, the accessor is sparse. Second dimension is the same size
       as `data` second dimension. */
    Containers::StridedArrayView2D<const char> sparseValues;
};

namespace {
    struct Sampler {
        SamplerFilter minificationFilter;
        SamplerFilter magnificationFilter;
        SamplerMipmap mipmap;
        Math::Vector3<SamplerWrapping> wrapping;
    };
}

struct GltfImporter::Document {
    /* Set only if fromFile() was used, passed to Utility::Json for nicer error
       messages and used as a base path for buffer and image opening */
    Containers::Optional<Containers::String> filename;

    /* File data, to which point parsed glTF tokens and the BIN chunk, if
       present */
    Containers::Array<char> fileData;
    Containers::Optional<Utility::Json> gltf;
    Containers::Optional<Containers::ArrayView<const char>> binChunk;

    /* Constant-time access to glTF data and their names. All these are checked
       to be object tokens during the initial import. Buffers, buffer views,
       accessors and samplers have names defined as well but we don't provide
       access to those, so no point in saving them. */
    Containers::Array<Containers::Reference<const Utility::JsonTokenData>>
        gltfBuffers,
        gltfBufferViews,
        gltfAccessors,
        gltfSamplers;
    Containers::Array<Containers::Pair<Containers::Reference<const Utility::JsonTokenData>, Containers::StringView>>
        gltfNodes,
        gltfMeshes, /* plus gltfMeshPrimitiveMap below */
        gltfCameras,
        gltfLights,
        gltfAnimations,
        gltfSkins,
        gltfImages,
        gltfTextures,
        gltfMaterials,
        gltfScenes;

    /* Storage for buffer content. If a buffer is fetched from a file callback,
       it's a non-owning view. These are filled on demand. We don't check for
       duplicate URIs since that's incredibly unlikely and hard to get right,
       so the buffer id is used as the index.

       If a buffer failed to load, it'll stay a NullOpt, meaning the same
       failure message will be printed next time it's accessed. If the buffer
       is empty and has no URI or it's the implicit buffer of a *.glb, it's
       NullOpt as well. */
    Containers::Array<Containers::Optional<Containers::Array<char>>> buffers;
    /* Parsed and validated buffer views. Same as with buffers, if any of these
       failed to validate, it'll stay a NullOpt, meaning the same failure
       message will be printed next time it's accessed. */
    Containers::Array<Containers::Optional<BufferView>> bufferViews;
    /* Parsed and validated accessors. Same as with buffers and buffer views,
       if any of these failed to validate, it'll stay a NullOpt, meaning the
       same failure message will be printed next time it's accessed.

       We're abusing VertexFormat here because it can describe all types
       supported by glTF including aligned matrices and because there's a
       builtin way to create a composite type out of component type,
       component/vector count and the normalized bit. Error messages print it
       without the VertexFormat:: prefix to avoid confusion, yet I think saying
       something like "Vector3ubNormalized is not a supported normal format" is
       better than "normalized VEC3 of 5121 is not a supported normal format"
       no matter how well formatted. */
    Containers::Array<Containers::Optional<Accessor>> accessors;
    /* Cached parsed samplers. Values left uninitialized, they will be set to
       appropriate default values inside doTexture(). */
    Containers::Array<Containers::Optional<Sampler>> samplers;

    /* Textures without duplicates from KHR_texture_ktx that point to the same
       image but have a different layer. IDs pointing to the gltfTextures
       array. */
    Containers::Array<UnsignedInt> uniqueTextures;
    /* Maps from the glTF texture ID (referenced by a material) to index in
       `uniqueTextures`. */
    Containers::Array<UnsignedInt> uniqueTextureForGltfTexture;
    /* Images partitioned by dimensions, first `image2DCount` is 2D images,
       then 3D images. IDs pointing to the gltfImages array. */
    Containers::Array<UnsignedInt> imagesByDimension;
    /* Maps from the glTF image ID (referenced by a texture) to index in
       `imagesByDimension`. */
    Containers::Array<UnsignedInt> imageByDimensionForGltfImage;
    std::size_t image2DCount;

    /* We can use StringView as the map key here because all views point to
       strings stored inside Utility::Json which ensures the pointers are
       stable and won't go out of scope. */
    Containers::Optional<std::unordered_map<Containers::StringView, Int>>
        animationsForName,
        camerasForName,
        lightsForName,
        scenesForName,
        skinsForName,
        nodesForName,
        meshesForName,
        materialsForName,
        images2DForName,
        images3DForName,
        texturesForName;

    /* Unlike the ones above, these are filled already during construction as
       we need them in three different places and on-demand construction would
       be too annoying to test. The key has to be a full string and not views
       on object keys inside the Json instance because it's joined with dots
       for nested objects. */
    std::unordered_map<Containers::String, SceneField> sceneFieldsForName;
    std::unordered_map<Containers::StringView, MeshAttribute> meshAttributesForName{
        #ifdef MAGNUM_BUILD_DEPRECATED
         /* Added as aliases of MeshAttribute::JointIds and
            MeshAttribute::Weights for backwards compatibility with code that
            used skinning even before there was builtin support for it.
            Wouldn't strictly need to be present if the file has no skinning
            meshes but having them present in the map always makes the
            implementation simpler. */
        {"JOINTS"_s, meshAttributeCustom(0)},
        {"WEIGHTS"_s, meshAttributeCustom(1)}
        #endif
    };
    /* The string views point to keys in the sceneFieldsForName map, for which
       STL guarantees iterator stability, i.e. the strings don't get moved
       anywhere even with SSO */
    Containers::Array<Containers::Triple<Containers::StringView, SceneFieldType, SceneFieldFlags>> sceneFieldNamesTypesFlags;
    Containers::Array<Containers::StringView> meshAttributeNames{InPlaceInit, {
        #ifdef MAGNUM_BUILD_DEPRECATED
        "JOINTS"_s,
        "WEIGHTS"_s
        #endif
    }};

    /* Mapping for multi-primitive meshes:

        -   gltfMeshPrimitiveMap.size() is the count of meshes reported to the
            user
        -   meshSizeOffsets.size() is the count of original meshes in the file
        -   gltfMeshPrimitiveMap[id] is a pair of (original mesh ID, glTF
            primitive token); the primitive token is checked to be an object
            token during the initial import
        -   meshSizeOffsets[j] points to the first item in gltfMeshPrimitiveMap
            for original mesh ID `j` -- which also translates the original ID
            to reported ID
        -   meshSizeOffsets[j + 1] - meshSizeOffsets[j] is count of meshes for
            original mesh ID `j` (or number of primitives in given mesh)
    */
    Containers::Array<Containers::Pair<std::size_t, Containers::Reference<const Utility::JsonTokenData>>> gltfMeshPrimitiveMap;
    Containers::Array<std::size_t> meshSizeOffsets;

    /* If a file contains texture coordinates that are not floats or normalized
       in the 0-1, the textureCoordinateYFlipInMaterial option is enabled
       implicitly as we can't perform Y-flip directly on the data. */
    bool textureCoordinateYFlipInMaterial = false;

    UnsignedInt imageImporterId = ~UnsignedInt{};
    Containers::Optional<AnyImageImporter> imageImporter;
};

Containers::Optional<Containers::Array<char>> GltfImporter::loadUri(const char* const errorPrefix, const Containers::StringView uri) {
    if(isDataUri(uri)) {
        /* Data URI with base64 payload according to RFC 2397:
           data:[<mediatype>][;base64],<data> */
        Containers::StringView base64;
        const Containers::Array3<Containers::StringView> parts = uri.partition(',');

        /* Non-base64 data URIs are allowed by RFC 2397, but make no sense for
           glTF */
        if(parts.front().hasSuffix(";base64"_s)) {
            /* This will be empty for both a missing comma and an empty payload */
            base64 = parts.back();
        }

        if(base64.isEmpty()) {
            Error{} << errorPrefix << "data URI has no base64 payload";
            return Containers::NullOpt;
        }

        return decodeBase64(errorPrefix, base64);
    }

    const Containers::Optional<Containers::String> decodedUri = decodeUri(errorPrefix, uri);
    if(!decodedUri)
        return {};

    if(fileCallback()) {
        const Containers::String fullPath = Utility::Path::join(_d->filename ? Utility::Path::path(*_d->filename) : Containers::StringView{}, *decodedUri);
        if(Containers::Optional<Containers::ArrayView<const char>> view = fileCallback()(fullPath, InputFileCallbackPolicy::LoadPermanent, fileCallbackUserData()))
            /* Return a non-owning view */
            return Containers::Array<char>{const_cast<char*>(view->data()), view->size(), [](char*, std::size_t){}};

        Error{} << errorPrefix << "error opening" << fullPath << "through a file callback";
        return {};

    } else {
        if(!_d->filename) {
            Error{} << errorPrefix << "external buffers can be imported only when opening files from the filesystem or if a file callback is present";
            return Containers::NullOpt;
        }

        const Containers::String fullPath = Utility::Path::join(Utility::Path::path(*_d->filename), *decodedUri);

        if(Containers::Optional<Containers::Array<char>> data = Utility::Path::read(fullPath))
            return data;

        Error{} << errorPrefix << "error opening" << fullPath;
        return {};
    }
}

Containers::Optional<Containers::ArrayView<const char>> GltfImporter::parseBuffer(const char* const errorPrefix, const UnsignedInt bufferId) {
    if(bufferId >= _d->gltfBuffers.size()) {
        Error{} << errorPrefix << "buffer index" << bufferId << "out of range for" << _d->gltfBuffers.size() << "buffers";
        return {};
    }

    Containers::Optional<Containers::Array<char>>& storage = _d->buffers[bufferId];
    if(storage)
        return Containers::ArrayView<const char>{*storage};

    const Utility::JsonToken gltfBuffer{*_d->gltf, _d->gltfBuffers[bufferId]};

    /* Each buffer object is accessed only once so it doesn't make sense to
       cache the parsed size */
    const Utility::JsonIterator gltfBufferByteLength = gltfBuffer.find("byteLength"_s);
    if(!gltfBufferByteLength || !_d->gltf->parseSize(*gltfBufferByteLength)) {
        Error{} << errorPrefix << "buffer" << bufferId
            << "has missing or invalid byteLength property";
        return {};
    }

    Containers::ArrayView<const char> view;
    if(const Utility::JsonIterator gltfBufferUri = gltfBuffer.find("uri"_s)) {
        if(!_d->gltf->parseString(*gltfBufferUri)) {
            Error{} << errorPrefix << "buffer" << bufferId << "has invalid uri property";
            return {};
        }
        if(!(storage = loadUri(errorPrefix, gltfBufferUri->asString())))
            return {};
        view = *storage;
    } else {
        /* URI has to be undefined for the first buffer referencing the glb
           binary blob. For others it should be present but we allow it to be
           omitted if the buffer has zero size. 3.6.1.2. (Binary Data Storage §
           Buffers and Buffer Views § GLB-stored Buffer) says that if it's
           missing, behavior is undefined / reserved for future extensions. */
        if(bufferId != 0 || !_d->binChunk) {
            if(gltfBufferByteLength->asSize() != 0) {
                Error{} << errorPrefix << "buffer" << bufferId << "has missing uri property";
                return {};
            }
        } else view = *_d->binChunk;
    }

    /* The spec mentions that non-GLB buffer length can be greater than
       byteLength. GLB buffer chunks may also be up to 3 bytes larger than
       byteLength because of padding. So we can't check for equality. */
    if(view.size() < gltfBufferByteLength->asSize()) {
        Error{} << errorPrefix << "buffer" << bufferId << "is too short, expected"
            << gltfBufferByteLength->asSize() << "bytes but got" << view.size();
        return {};
    }

    return view;
}

Containers::Optional<GltfImporter::BufferView> GltfImporter::parseBufferView(const char* const errorPrefix, const UnsignedInt bufferViewId) {
    if(bufferViewId >= _d->gltfBufferViews.size()) {
        Error{} << errorPrefix << "buffer view index" << bufferViewId << "out of range for" << _d->gltfBufferViews.size() << "buffer views";
        return {};
    }

    /* Return if the buffer view is already parsed */
    Containers::Optional<BufferView>& storage = _d->bufferViews[bufferViewId];
    if(storage)
        return storage;

    const Utility::JsonToken gltfBufferView{*_d->gltf, _d->gltfBufferViews[bufferViewId]};
    const Utility::JsonIterator gltfBufferId = gltfBufferView.find("buffer"_s);
    if(!gltfBufferId || !(_d->gltf->parseUnsignedInt(*gltfBufferId))) {
        Error{} << errorPrefix << "buffer view" << bufferViewId
            << "has missing or invalid buffer property";
        return {};
    }

    /* Get the buffer early and continue only if that doesn't fail. This also
       checks that the buffer ID is in bounds. */
    Containers::Optional<Containers::ArrayView<const char>> buffer = parseBuffer(errorPrefix, gltfBufferId->asUnsignedInt());
    if(!buffer)
        return {};

    /* Byte offset is optional, defaulting to 0 */
    const Utility::JsonIterator gltfByteOffset = gltfBufferView.find("byteOffset"_s);
    if(gltfByteOffset && !_d->gltf->parseSize(*gltfByteOffset)) {
        Error{} << errorPrefix << "buffer view" << bufferViewId << "has invalid byteOffset property";
        return {};
    }

    const Utility::JsonIterator gltfByteLength = gltfBufferView.find("byteLength"_s);
    if(!gltfByteLength || !_d->gltf->parseSize(*gltfByteLength)) {
        Error{} << errorPrefix << "buffer view" << bufferViewId << "has missing or invalid byteLength property";
        return {};
    }

    /* Byte stride is optional, if not set it's tightly packed. Assuming it's
       not larger than 4 GB -- glTF itself has the limit much lower (252, heh),
       but we don't really need to go that low. */
    const Utility::JsonIterator gltfByteStride = gltfBufferView.find("byteStride"_s);
    if(gltfByteStride && !_d->gltf->parseUnsignedInt(*gltfByteStride)) {
        Error{} << errorPrefix << "buffer view" << bufferViewId << "has invalid byteStride property";
        return {};
    }

    const std::size_t offset = gltfByteOffset ? gltfByteOffset->asSize() : 0;
    const std::size_t requiredBufferSize = offset + gltfByteLength->asSize();
    if(buffer->size() < requiredBufferSize) {
        Error{} << errorPrefix << "buffer view" << bufferViewId << "needs" << requiredBufferSize << "bytes but buffer" << gltfBufferId->asUnsignedInt() << "has only" << buffer->size();
        return {};
    }

    /* If the buffer isn't strided, the first dimension has a zero stride and
       the second is the whole view */
    storage.emplace(
        buffer->slice(offset, offset + gltfByteLength->asSize()),
        gltfByteStride ? gltfByteStride->asUnsignedInt() : 0,
        gltfBufferId->asUnsignedInt());

    return storage;
}

Containers::Optional<GltfImporter::Accessor> GltfImporter::parseAccessor(const char* const errorPrefix, const UnsignedInt accessorId) {
    if(accessorId >= _d->gltfAccessors.size()) {
        Error{} << errorPrefix << "accessor index" << accessorId << "out of range for" << _d->gltfAccessors.size() << "accessors";
        return {};
    }

    /* Return if the buffer view is already parsed */
    Containers::Optional<Accessor>& storage = _d->accessors[accessorId];
    if(storage)
        return storage;

    const Utility::JsonToken gltfAccessor{*_d->gltf, _d->gltfAccessors[accessorId]};

    /** @todo Validate alignment rules, calculate correct stride in accessorView():
        https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#data-alignment */

    const Utility::JsonIterator gltfAccessorComponentType = gltfAccessor.find("componentType"_s);
    if(!gltfAccessorComponentType || !_d->gltf->parseUnsignedInt(*gltfAccessorComponentType)) {
        Error{} << errorPrefix << "accessor" << accessorId << "has missing or invalid componentType property";
        return {};
    }
    VertexFormat componentFormat;
    switch(gltfAccessorComponentType->asUnsignedInt()) {
        case Implementation::GltfTypeByte:
            componentFormat = VertexFormat::Byte;
            break;
        case Implementation::GltfTypeUnsignedByte:
            componentFormat = VertexFormat::UnsignedByte;
            break;
        case Implementation::GltfTypeShort:
            componentFormat = VertexFormat::Short;
            break;
        case Implementation::GltfTypeUnsignedShort:
            componentFormat = VertexFormat::UnsignedShort;
            break;
        /* Signed int not supported in glTF at the moment */
        case Implementation::GltfTypeUnsignedInt:
            componentFormat = VertexFormat::UnsignedInt;
            break;
        case Implementation::GltfTypeFloat:
            componentFormat = VertexFormat::Float;
            break;
        default:
            Error{} << errorPrefix << "accessor" << accessorId << "has invalid componentType" << gltfAccessorComponentType->asUnsignedInt();
            return {};
    }

    const Utility::JsonIterator gltfAccessorCount = gltfAccessor.find("count"_s);
    if(!gltfAccessorCount || !_d->gltf->parseSize(*gltfAccessorCount)) {
        Error{} << errorPrefix << "accessor" << accessorId << "has missing or invalid count property";
        return {};
    }
    const std::size_t count = gltfAccessorCount->asSize();

    const Utility::JsonIterator gltfAccessorType = gltfAccessor.find("type"_s);
    if(!gltfAccessorType || !_d->gltf->parseString(*gltfAccessorType)) {
        Error{} << errorPrefix << "accessor" << accessorId << "has missing or invalid type property";
        return {};
    }
    const Containers::StringView accessorType = gltfAccessorType->asString();
    UnsignedInt componentCount, vectorCount;
    if(accessorType == "SCALAR"_s) {
        componentCount = 1;
        vectorCount = 1;
    } else if(accessorType == "VEC2"_s) {
        componentCount = 2;
        vectorCount = 1;
    } else if(accessorType == "VEC3"_s) {
        componentCount = 3;
        vectorCount = 1;
    } else if(accessorType == "VEC4"_s) {
        componentCount = 4;
        vectorCount = 1;
    } else if(accessorType == "MAT2"_s) {
        componentCount = vectorCount = 2;
    } else if(accessorType == "MAT3"_s) {
        componentCount = vectorCount = 3;
    } else if(accessorType == "MAT4"_s) {
        componentCount = vectorCount = 4;
    } else {
        Error{} << errorPrefix << "accessor" << accessorId << "has invalid type" << accessorType;
        return {};
    }

    /* Normalized is optional, defaulting to false */
    const Utility::JsonIterator gltfAccessorNormalized = gltfAccessor.find("normalized"_s);
    if(gltfAccessorNormalized && !_d->gltf->parseBool(*gltfAccessorNormalized)) {
        Error{} << errorPrefix << "accessor" << accessorId << "has invalid normalized property";
        return {};
    }

    /* Check for illegal normalized types */
    if(gltfAccessorNormalized && gltfAccessorNormalized->asBool()) {
        if(componentFormat == VertexFormat::UnsignedInt ||
           componentFormat == VertexFormat::Float) {
            /* Since we're abusing VertexFormat for all formats, print just the
               enum value without the prefix to avoid cofusion */
            Error{} << errorPrefix << "accessor" << accessorId << "with component format" << Debug::packed << componentFormat << "can't be normalized";
            return {};
        }
    }

    /* We have only few allowed matrix types */
    if(vectorCount != 1 && componentFormat != VertexFormat::Float &&
        !(componentFormat == VertexFormat::Byte && gltfAccessorNormalized && gltfAccessorNormalized->asBool()) &&
        !(componentFormat == VertexFormat::Short && gltfAccessorNormalized && gltfAccessorNormalized->asBool())
    ) {
        /* Compose the normalized bit into the component format for printing.
           This shouldn't assert as we checked for illegal normalized types
           right above. Also, since we're abusing VertexFormat for all formats,
           print just the enum value without the prefix to avoid cofusion. */
        Error{} << errorPrefix << "accessor" << accessorId << "has an unsupported matrix component format" << Debug::packed << vertexFormat(componentFormat, 1, gltfAccessorNormalized && gltfAccessorNormalized->asBool());
        return {};
    }

    VertexFormat format;
    if(vectorCount == 1)
        format = vertexFormat(componentFormat, componentCount, gltfAccessorNormalized && gltfAccessorNormalized->asBool());
    else
        format = vertexFormat(componentFormat, vectorCount, componentCount, true);
    const std::size_t typeSize = vertexFormatSize(format);

    /* Buffer views are optional in accessors, we're supposed to fill the view
       with zeros if they're missing */
    Containers::StridedArrayView2D<const char> data;
    const Utility::JsonIterator gltfBufferViewId = gltfAccessor.find("bufferView"_s);
    if(gltfBufferViewId) {
        if(!_d->gltf->parseUnsignedInt(*gltfBufferViewId)) {
            Error{} << errorPrefix << "accessor" << accessorId << "has invalid bufferView property";
            return {};
        }

        /* Get the buffer view and continue only if that doesn't fail. This
           also checks that the buffer view ID is in bounds. */
        Containers::Optional<BufferView> bufferView = parseBufferView(errorPrefix, gltfBufferViewId->asUnsignedInt());
        if(!bufferView)
            return {};

        if(bufferView->stride && bufferView->stride < typeSize) {
            Error{} << errorPrefix << typeSize << Debug::nospace << "-byte type defined by accessor" << accessorId << "can't fit into buffer view" << gltfBufferViewId->asUnsignedInt() << "stride of" << bufferView->stride;
            return {};
        }

        /* Byte offset is optional, defaulting to 0. The spec says this can
           only be defined if bufferView is also, which means the code should
           ideally parse & check it always, not just when bufferView is
           present. But this is a loader, not validator, so I don't see a
           practical reason for doing that. */
        const Utility::JsonIterator gltfAccessorByteOffset = gltfAccessor.find("byteOffset"_s);
        if(gltfAccessorByteOffset && !_d->gltf->parseSize(*gltfAccessorByteOffset)) {
            Error{} << errorPrefix << "accessor" << accessorId << "has invalid byteOffset property";
            return {};
        }

        const std::size_t offset = gltfAccessorByteOffset ? gltfAccessorByteOffset->asSize() : 0;
        const std::size_t stride = bufferView->stride ? bufferView->stride : typeSize;
        const std::size_t requiredBufferViewSize = offset + (count ? stride*(count - 1) + typeSize : 0);
        if(bufferView->data.size() < requiredBufferViewSize) {
            Error{} << errorPrefix << "accessor" << accessorId << "needs" << requiredBufferViewSize << "bytes but buffer view" << gltfBufferViewId->asUnsignedInt() << "has only" << bufferView->data.size();
            return {};
        }

        /* glTF only requires buffer views to be large enough to fit the actual
           data, not to have the size large enough to fit `count*stride`
           elements. The StridedArrayView expects the latter, so we fake the
           vertexData size to satisfy the assert. For simplicity we overextend
           by the whole stride instead of `offset + typeSize`, relying on
           on the above bound checks. A similar workaround is in doMesh() when
           populating mesh attribute data. */
        /** @todo instead of faking the size, split the offset into offset in
            whole strides and the remainder (Math::div), then form the view
            with offset in whole strides and then "shift" the view by the
            remainder (once there's StridedArrayView::shift() or some such) */
        data = Containers::StridedArrayView2D<const char>{
            {bufferView->data, bufferView->data.size() + stride},
            bufferView->data.data() + offset,
            {count, typeSize},
            {std::ptrdiff_t(stride), 1}};

    /* If there's no buffer view, make a null data view but with its size
       describing the element count and type size. In the output it will become
       zero-initialized, possibly with sparse data put on top. */
    } else data = Containers::StridedArrayView2D<const char>{
        {nullptr, count*typeSize},
        {count, typeSize}};

    /* Sparse accessor, if any */
    Containers::StridedArrayView2D<const char> sparseIndices, sparseValues;
    if(const Utility::JsonIterator gltfSparse = gltfAccessor.find("sparse"_s)) {
        if(!_d->gltf->parseObject(*gltfSparse)) {
            Error{} << errorPrefix << "accessor" << accessorId << "has invalid sparse property";
            return {};
        }

        /* Count of sparse values */
        const Utility::JsonIterator gltfSparseCount = gltfSparse->find("count"_s);
        if(!gltfSparseCount || !_d->gltf->parseSize(*gltfSparseCount)) {
            Error{} << errorPrefix << "accessor" << accessorId << "has missing or invalid sparse count property";
            return {};
        }

        const std::size_t sparseCount = gltfSparseCount->asSize();
        if(!sparseCount || sparseCount > count) {
            Error{} << errorPrefix << "accessor" << accessorId << "sparse count" << sparseCount << "out of range for" << count << "elements";
            return {};
        }

        /* Sparse indices */
        const Utility::JsonIterator gltfSparseIndices = gltfSparse->find("indices");
        if(!gltfSparseIndices || !_d->gltf->parseObject(*gltfSparseIndices)) {
            Error{} << errorPrefix << "accessor" << accessorId << "has missing or invalid sparse indices property";
            return {};
        }

        const Utility::JsonIterator gltfSparseIndicesBufferViewId = gltfSparseIndices->find("bufferView");
        if(!gltfSparseIndicesBufferViewId || !_d->gltf->parseUnsignedInt(*gltfSparseIndicesBufferViewId)) {
            Error{} << errorPrefix << "accessor" << accessorId << "has missing or invalid sparse indices bufferView property";
            return {};
        }

        /* Get the buffer view and continue only if that doesn't fail. This
           also checks that the buffer view ID is in bounds. */
        Containers::Optional<BufferView> sparseIndicesBufferView = parseBufferView(errorPrefix, gltfSparseIndicesBufferViewId->asUnsignedInt());
        if(!sparseIndicesBufferView)
            return {};

        /* 5.3 (Accessor Sparse Indices) says strided buffer views aren't
           allowed for sparse indices, even if the stride would equal type
           size. Doesn't matter much as I can use a StridedArrayView anyway but
           it definitely makes certain things simpler. */
        if(sparseIndicesBufferView->stride) {
            Error{} << errorPrefix << "accessor" << accessorId << "sparse indices bufferView" << gltfSparseIndicesBufferViewId->asUnsignedInt() << "is strided";
            return {};
        }

        /* Byte offset is optional, defaulting to 0 */
        const Utility::JsonIterator gltfSparseIndicesByteOffset = gltfSparseIndices->find("byteOffset"_s);
        if(gltfSparseIndicesByteOffset && !_d->gltf->parseSize(*gltfSparseIndicesByteOffset)) {
            Error{} << errorPrefix << "accessor" << accessorId << "has invalid sparse indices byteOffset property";
            return {};
        }

        const Utility::JsonIterator gltfSparseIndicesComponentType = gltfSparseIndices->find("componentType"_s);
        if(!gltfSparseIndicesComponentType || !_d->gltf->parseUnsignedInt(*gltfSparseIndicesComponentType)) {
            Error{} << errorPrefix << "accessor" << accessorId << "has missing or invalid sparse indices componentType property";
            return {};
        }
        UnsignedInt sparseIndicesTypeSize;
        switch(gltfSparseIndicesComponentType->asUnsignedInt()) {
            case Implementation::GltfTypeUnsignedByte:
                sparseIndicesTypeSize = 1;
                break;
            case Implementation::GltfTypeUnsignedShort:
                sparseIndicesTypeSize = 2;
                break;
            case Implementation::GltfTypeUnsignedInt:
                sparseIndicesTypeSize = 4;
                break;
            default:
                Error{} << errorPrefix << "accessor" << accessorId << "has invalid sparse indices componentType" << gltfSparseIndicesComponentType->asUnsignedInt();
                return {};
        }

        const std::size_t sparseIndicesOffset = gltfSparseIndicesByteOffset ? gltfSparseIndicesByteOffset->asSize() : 0;
        const std::size_t requiredSparseIndicesBufferViewSize = sparseIndicesOffset + sparseCount*sparseIndicesTypeSize;
        if(sparseIndicesBufferView->data.size() < requiredSparseIndicesBufferViewSize) {
            Error{} << errorPrefix << "accessor" << accessorId << "needs" << requiredSparseIndicesBufferViewSize << "bytes for sparse indices but buffer view" << gltfSparseIndicesBufferViewId->asUnsignedInt() << "has only" << sparseIndicesBufferView->data.size();
            return {};
        }

        /* Sparse values */
        const Utility::JsonIterator gltfSparseValues = gltfSparse->find("values");
        if(!gltfSparseValues || !_d->gltf->parseObject(*gltfSparseValues)) {
            Error{} << errorPrefix << "accessor" << accessorId << "has missing or invalid sparse values property";
            return {};
        }

        const Utility::JsonIterator gltfSparseValuesBufferViewId = gltfSparseValues->find("bufferView");
        if(!gltfSparseValuesBufferViewId || !_d->gltf->parseUnsignedInt(*gltfSparseValuesBufferViewId)) {
            Error{} << errorPrefix << "accessor" << accessorId << "has missing or invalid sparse values bufferView property";
            return {};
        }

        /* Get the buffer view and continue only if that doesn't fail. This
           also checks that the buffer view ID is in bounds. */
        Containers::Optional<BufferView> sparseValuesBufferView = parseBufferView(errorPrefix, gltfSparseValuesBufferViewId->asUnsignedInt());
        if(!sparseValuesBufferView)
            return {};

        /* 5.4 (Accessor Sparse Indices) similarly says strided buffer views
           aren't allowed for sparse values either. Again doesn't matter but
           makes things simpler. */
        if(sparseValuesBufferView->stride) {
            Error{} << errorPrefix << "accessor" << accessorId << "sparse values bufferView" << gltfSparseValuesBufferViewId->asUnsignedInt() << "is strided";
            return {};
        }

        /* Byte offset is optional, defaulting to 0 */
        const Utility::JsonIterator gltfSparseValuesByteOffset = gltfSparseValues->find("byteOffset"_s);
        if(gltfSparseValuesByteOffset && !_d->gltf->parseSize(*gltfSparseValuesByteOffset)) {
            Error{} << errorPrefix << "accessor" << accessorId << "has invalid sparse values byteOffset property";
            return {};
        }

        const std::size_t sparseValuesOffset = gltfSparseValuesByteOffset ? gltfSparseValuesByteOffset->asSize() : 0;
        const std::size_t requiredSparseValuesBufferViewSize = sparseValuesOffset + sparseCount*typeSize;
        if(sparseValuesBufferView->data.size() < requiredSparseValuesBufferViewSize) {
            Error{} << errorPrefix << "accessor" << accessorId << "needs" << requiredSparseValuesBufferViewSize << "bytes for sparse indices but buffer view" << gltfSparseValuesBufferViewId->asUnsignedInt() << "has only" << sparseValuesBufferView->data.size();
            return {};
        }

        /* All good, form the views for storing below */
        sparseIndices = Containers::StridedArrayView2D<const char>{
            sparseIndicesBufferView->data.sliceSize(sparseIndicesOffset, sparseCount*sparseIndicesTypeSize),
            {sparseCount, sparseIndicesTypeSize}};
        sparseValues = Containers::StridedArrayView2D<const char>{
            sparseValuesBufferView->data.sliceSize(sparseValuesOffset, sparseCount*typeSize),
            {sparseCount, typeSize}};
    }

    storage.emplace(
        data,
        format,
        /* Use all 1s to denote the accessor has no buffer assigned */
        gltfBufferViewId ? gltfBufferViewId->asUnsignedInt() : ~UnsignedInt{},
        sparseIndices,
        sparseValues);

    return storage;
}

namespace {

/* A variant of MeshTools::duplicateIntoImplementation(), just modified to have
   the indices point to the output instead of input and user-facing assertions
   turned internal */
template<class T> bool applySparseAccessor(const char* const errorPrefix, UnsignedInt accessorId, const Containers::StridedArrayView1D<const T>& indices, const Containers::StridedArrayView2D<const char>& data, const Containers::StridedArrayView2D<char>& out) {
    CORRADE_INTERNAL_ASSERT(
        indices.size() == data.size()[0] &&
        data.isContiguous<1>() &&
        out.isContiguous<1>() &&
        data.size()[1] == out.size()[1]);
    const std::size_t size = data.size()[1];
    for(std::size_t i = 0; i != indices.size(); ++i) {
        const std::size_t index = indices[i];
        if(index >= out.size()[0]) {
            Error{} << errorPrefix << "sparse accessor" << accessorId << "index" << index << "out of range for" << out.size()[0] << "elements";
            return false;
        }
        std::memcpy(out[index].data(), data[i].data(), size);
    }
    return true;
}

}

#ifdef MAGNUM_BUILD_DEPRECATED /* LCOV_EXCL_START */
namespace {

/* Only used by the deprecated constructors now */
void fillDefaultConfiguration(Utility::ConfigurationGroup& conf) {
    conf.setValue("ignoreRequiredExtensions", false);
    conf.setValue("optimizeQuaternionShortestPath", true);
    conf.setValue("normalizeQuaternions", true);
    conf.setValue("mergeAnimationClips", false);
    conf.setValue("phongMaterialFallback", true);
    conf.setValue("objectIdAttribute", "_OBJECT_ID");
}

}

GltfImporter::GltfImporter() {
    fillDefaultConfiguration(configuration());
}

GltfImporter::GltfImporter(PluginManager::Manager<AbstractImporter>& manager): AbstractImporter{manager} {
    fillDefaultConfiguration(configuration());
}
#endif /* LCOV_EXCL_STOP */

GltfImporter::GltfImporter(PluginManager::AbstractManager& manager, const Containers::StringView& plugin): AbstractImporter{manager, plugin} {}

GltfImporter::~GltfImporter() = default;

ImporterFeatures GltfImporter::doFeatures() const { return ImporterFeature::OpenData|ImporterFeature::FileCallback; }

bool GltfImporter::doIsOpened() const { return !!_d && _d->gltf; }

void GltfImporter::doClose() { _d = nullptr; }

void GltfImporter::doOpenFile(const Containers::StringView filename) {
    _d.reset(new Document);
    _d->filename.emplace(Containers::String::nullTerminatedGlobalView(filename));
    AbstractImporter::doOpenFile(filename);
}

namespace {

bool isRecognized2DTextureExtension(Containers::StringView name) {
    return
        /* KHR_texture_basisu allows the usage of mimeType image/ktx2 but only
           explicitly talks about KTX2 with Basis compression. We neither care
           nor check that. */
        name == "KHR_texture_basisu"_s  ||
        /* GOOGLE_texture_basis is not a registered extension but can be found
           in some of the early Basis Universal examples. Basis files don't
           have a registered mimetype either, but as explained above we don't
           care about mimetype at all. */
        name == "GOOGLE_texture_basis"_s ||
        /* Similarly, these extensions also only allows DDS / WebP files to be
           referenced from it. We don't care, again. */
        name == "MSFT_texture_dds"_s ||
        name == "EXT_texture_webp"_s;
}

/* Used by doOpenData() but it's recursive and so it can't be a local lambda */
bool discoverSceneExtraFields(Utility::Json& gltf, std::unordered_map<Containers::String, SceneField>& sceneFieldsForName, Containers::Array<Containers::Triple<Containers::StringView, SceneFieldType, SceneFieldFlags>>& sceneFieldNamesTypesFlags, const Utility::ConfigurationGroup* const customSceneFieldTypeConfiguration, UnsignedInt nodeI, const Containers::StringView key, Utility::JsonToken gltfExtraValue) {
    /* If the value is an object, recurse into it. The field name will then be
       all object keys concatenated with dots. */
    if(gltfExtraValue.type() == Utility::JsonToken::Type::Object) {
        /* If the object fails to parse because it has invalid keys (i.e.,
           invalid Unicode escapes), fail the whole import. If we wouldn't,
           it'd still print a message to the output which would imply an error
           was silently ignored, which is not any better. */
        if(!gltf.parseObject(gltfExtraValue)) {
            Error{} << "Trade::GltfImporter::openData(): invalid node" << nodeI << "extras property";
            return false;
        }

        for(const Utility::JsonObjectItem gltfNestedExtra: gltfExtraValue.asObject()) {
            if(!discoverSceneExtraFields(
                gltf, sceneFieldsForName, sceneFieldNamesTypesFlags,
                customSceneFieldTypeConfiguration,
                nodeI, "."_s.join({key, gltfNestedExtra.key()}), gltfNestedExtra.value())
            )
                return false;
        }

        return true;
    }

    /* Get token type. If the extra field is an array, we can parse it if all
       items are of a common type. If not, skip it -- a warning about a
       non-homogeneous type will be printed in doScene(). */
    Utility::JsonToken::Type tokenType;
    bool isArray;
    if(gltfExtraValue.type() == Utility::JsonToken::Type::Array) {
        /** @todo this skips also empty arrays, which then don't have the key
            registered among custom scene fields which may break workflows that
            rely on it being present even if all uses of it are an empty array
            -- add a placeholder with "unknown type" instead and set it once a
            non-empty array or a non-array value is found */
        if(const Containers::Optional<Utility::JsonToken::Type> commonType = gltfExtraValue.commonArrayType())
            tokenType = *commonType;
        else return true;
        isArray = true;
    } else {
        tokenType = gltfExtraValue.type();
        isArray = false;
    }

    /* For the actual type or common array type skip everything that's not a
       bool, number or a string. A warning about a skipped extra due to
       unsupported type will be printed in doScene(). */
    if(tokenType != Utility::JsonToken::Type::Bool &&
       tokenType != Utility::JsonToken::Type::Number &&
       tokenType != Utility::JsonToken::Type::String)
        return true;

    const auto inserted = sceneFieldsForName.emplace(key, sceneFieldCustom(sceneFieldNamesTypesFlags.size()));
    if(inserted.second) {
        /* If the field has the type specified in configuration,
           override the default */
        /** @todo use findValue() once the Configuration API is reworked,
            instead of treating empty option the same as no option at all */
        const Containers::StringView typeString = customSceneFieldTypeConfiguration ? customSceneFieldTypeConfiguration->value<Containers::StringView>(key) : ""_s;
        SceneFieldType type;
        if(!typeString) switch(tokenType) {
            case Utility::JsonToken::Type::Bool:
                type = SceneFieldType::Bit;
                break;
            case Utility::JsonToken::Type::Number:
                type = SceneFieldType::Float;
                break;
            case Utility::JsonToken::Type::String:
                type = SceneFieldType::StringOffset32;
                break;
            default: CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
        }
        #define _c(type_) if(typeString == #type_ ## _s) type = SceneFieldType::type_;
        else _c(Float)
        else _c(UnsignedInt)
        else _c(Int)
        #undef _c
        else {
            /* I expect the type set to grow significantly over time, thus
               listing them all in the error message doesn't scale */
            Error{} << "Trade::GltfImporter::openData(): invalid type" << typeString << "specified for custom scene field" << key;
            return false;
        }

        arrayAppend(sceneFieldNamesTypesFlags, InPlaceInit,
            /* Referencing the string stored in the hashmap, which should not
               change place as the iterators are guaranteed to be stable. This
               way sceneFieldNamesTypesFlags can store only views and not
               copies of the strings again. */
            inserted.first->first,
            type,
            isArray ? SceneFieldFlag::MultiEntry : SceneFieldFlags{});
    }

    return true;
}

}

void GltfImporter::doOpenData(Containers::Array<char>&& data, const DataFlags dataFlags) {
    if(!_d)
        _d.reset(new Document);

    /* Copy file content. Take over the existing array or copy the data if we
       can't. We need to keep the data around as JSON tokens are views onto it
       and also for the GLB binary chunk. */
    if(dataFlags & (DataFlag::Owned|DataFlag::ExternallyOwned)) {
        _d->fileData = Utility::move(data);
    } else {
        _d->fileData = Containers::Array<char>{NoInit, data.size()};
        Utility::copy(data, _d->fileData);
    }

    /* Since we just made a owning copy of the file data above, mark the JSON
       string view as global to avoid Utility::Json making its own owned copy
       again */
    Containers::StringView json{_d->fileData, _d->fileData.size(), Containers::StringViewFlag::Global};
    std::size_t jsonByteOffset = 0;

    /* If the file looks like a GLB, extract the JSON and BIN chunk out of it */
    if(json.hasPrefix("glTF"_s)) {
        if(_d->fileData.size() < sizeof(Implementation::GltfGlbHeader)) {
            Error{} << "Trade::GltfImporter::openData(): binary glTF too small, expected at least" << sizeof(Implementation::GltfGlbHeader) << "bytes but got only" << _d->fileData.size();
            return;
        }
        const auto& header = *reinterpret_cast<const Implementation::GltfGlbHeader*>(_d->fileData.data());
        if(header.version != 2) {
            Error{} << "Trade::GltfImporter::openData(): unsupported binary glTF version" << header.version;
            return;
        }
        if(_d->fileData.size() != header.length) {
            Error{} << "Trade::GltfImporter::openData(): binary glTF size mismatch, expected" << header.length << "bytes but got" << _d->fileData.size();
            return;
        }
        if(Containers::StringView{header.json.magic, 4} != "JSON"_s) {
            /** @todo use Debug::str (escaping non-printable characters)
                instead of the hex once it exists */
            Error{} << "Trade::GltfImporter::openData(): expected a JSON chunk, got" << Debug::hex << header.json.id;
            return;
        }

        const char* const jsonDataBegin = _d->fileData + sizeof(Implementation::GltfGlbHeader);
        const char* const jsonDataEnd = jsonDataBegin + header.json.length;
        if(jsonDataEnd > _d->fileData.end()) {
            Error{} << "Trade::GltfImporter::openData(): binary glTF size mismatch, expected" << header.json.length << "bytes for a JSON chunk but got only" << _d->fileData.end() - jsonDataBegin;
            return;
        }

        /* Update the JSON view to contain just the JSON data. Slicing so the
           global flag set above gets preserved. */
        json = json.slice(jsonDataBegin, jsonDataBegin + header.json.length);
        jsonByteOffset = jsonDataBegin - _d->fileData;

        /* Other chunks. The spec defines just the BIN chunk, but there can be
           additional chunks defined by extensions that we're expected to
           skip */
        const char* chunk = jsonDataEnd;
        while(chunk != _d->fileData.end()) {
            if(chunk + sizeof(Implementation::GltfGlbChunkHeader) > _d->fileData.end()) {
                Error{} << "Trade::GltfImporter::openData(): binary glTF chunk starting at" << chunk - _d->fileData.begin() << "too small, expected at least" << sizeof(Implementation::GltfGlbChunkHeader) << "bytes but got only" << _d->fileData.end() - chunk;
                return;
            }

            const auto& chunkHeader = *reinterpret_cast<const Implementation::GltfGlbChunkHeader*>(chunk);
            const char* const chunkDataBegin = chunk + sizeof(Implementation::GltfGlbChunkHeader);
            const char* const chunkDataEnd = chunkDataBegin + chunkHeader.length;
            if(chunkDataEnd > _d->fileData.end()) {
                Error{} << "Trade::GltfImporter::openData(): binary glTF size mismatch, expected" << chunkHeader.length << "bytes for a chunk starting at" << chunk - _d->fileData.begin() << "but got only" << _d->fileData.end() - chunkDataBegin;
                return;
            }

            /* If a BIN chunk, save it. There can be at most one, so a warning
               will be printed for the next ones */
            if(!_d->binChunk && Containers::StringView{chunkHeader.magic, 4} == "BIN\0"_s)
                _d->binChunk = Containers::arrayView(chunkDataBegin, chunkHeader.length);
            else if(!(flags() & ImporterFlag::Quiet))
                /** @todo use Debug::str (escaping non-printable characters)
                    instead of the hex once it exists */
                Warning{} << "Trade::GltfImporter::openData(): ignoring chunk" << Debug::hex << chunkHeader.id << "at" << chunk - _d->fileData.begin();
            chunk = chunkDataEnd;
        }
    }

    Containers::Optional<Utility::Json> gltf = Utility::Json::fromString(json, _d->filename ? Containers::StringView{*_d->filename} : Containers::StringView{}, 0, jsonByteOffset);
    if(!gltf || !gltf->parseObject(gltf->root())) {
        Error{} << "Trade::GltfImporter::openData(): invalid JSON";
        return;
    }

    /* Check version */
    const Utility::JsonIterator gltfAsset = gltf->root().find("asset"_s);
    if(!gltfAsset || !gltf->parseObject(*gltfAsset)) {
        Error{} << "Trade::GltfImporter::openData(): missing or invalid asset property";
        return;
    }
    const Utility::JsonIterator gltfAssetVersion = gltfAsset->find("version"_s);
    if(!gltfAssetVersion || !gltf->parseString(*gltfAssetVersion)) {
        Error{} << "Trade::GltfImporter::openData(): missing or invalid asset version property";
        return;
    }
    /* Min version is optional */
    const Utility::JsonIterator gltfAssetMinVersion = gltfAsset->find("minVersion"_s);
    if(gltfAssetMinVersion && !gltf->parseString(*gltfAssetMinVersion)) {
        Error{} << "Trade::GltfImporter::openData(): invalid asset minVersion property";
        return;
    }

    /* Major versions are forward- and backward-compatible, but minVersion can
       be used to require support for features added in new minor versions.
       So far there's only 2.0 so we can use an exact comparison. */
    if(gltfAssetMinVersion && gltfAssetMinVersion->asString() != "2.0"_s) {
        Error{} << "Trade::GltfImporter::openData(): unsupported minVersion" << gltfAssetMinVersion->asString() << Debug::nospace << ", expected 2.0";
        return;
    }
    if(!gltfAssetVersion->asString().hasPrefix("2."_s)) {
        Error{} << "Trade::GltfImporter::openData(): unsupported version" << gltfAssetVersion->asString() << Debug::nospace << ", expected 2.x";
        return;
    }

    /* Check used extensions for any experimental feature that's off by
       default and hint at it */
    if(const Utility::JsonIterator gltfExtensionsUsed = gltf->root().find("extensionsUsed"_s)) {
        if(!gltf->parseArray(*gltfExtensionsUsed)) {
            Error{} << "Trade::GltfImporter::openData(): invalid extensionsUsed property";
            return;
        }

        for(Utility::JsonArrayItem gltfExtension: gltfExtensionsUsed->asArray()) {
            if(!gltf->parseString(gltfExtension)) {
                Error{} << "Trade::GltfImporter::openData(): invalid used extension" << gltfExtension.index();
                return;
            }

            if(!(flags() & ImporterFlag::Quiet) && gltfExtension.value().asString() == "KHR_texture_ktx"_s && !configuration().value<bool>("experimentalKhrTextureKtx"))
                Warning{} << "Trade::GltfImporter::openData(): used extension KHR_texture_ktx is experimental, enable experimentalKhrTextureKtx to use it";
        }
    }

    /* Check required extensions. Every extension in extensionsRequired is
       required to "load and/or render an asset". */
    if(const Utility::JsonIterator gltfExtensionsRequired = gltf->root().find("extensionsRequired"_s)) {
        if(!gltf->parseArray(*gltfExtensionsRequired)) {
            Error{} << "Trade::GltfImporter::openData(): invalid extensionsRequired property";
            return;
        }

        /** @todo Allow ignoring specific extensions through a config option,
            e.g. ignoreRequiredExtension=KHR_materials_volume */
        const bool ignoreRequiredExtensions = configuration().value<bool>("ignoreRequiredExtensions");

        Containers::Array<Containers::StringView> supportedExtensions;
        arrayAppend(supportedExtensions, {
            "KHR_lights_punctual"_s,
            "KHR_materials_clearcoat"_s,
            "KHR_materials_pbrSpecularGlossiness"_s,
            "KHR_materials_unlit"_s,
            "KHR_mesh_quantization"_s,
            "KHR_texture_basisu"_s,
            "KHR_texture_transform"_s,
            "GOOGLE_texture_basis"_s,
            "MSFT_texture_dds"_s,
            "EXT_texture_webp"_s
        });
        if(configuration().value<bool>("experimentalKhrTextureKtx"))
            arrayAppend(supportedExtensions, "KHR_texture_ktx"_s);

        /* M*N loop should be okay here, extensionsRequired should usually have
           no or very few entries. Consider binary search if the list of
           supported extensions reaches a few dozen. */
        for(Utility::JsonArrayItem gltfExtension: gltfExtensionsRequired->asArray()) {
            if(!gltf->parseString(gltfExtension)) {
                Error{} << "Trade::GltfImporter::openData(): invalid required extension" << gltfExtension.index();
                return;
            }

            const Containers::StringView extension = gltfExtension.value().asString();
            bool found = false;
            for(const auto& supported: supportedExtensions) {
                if(supported == extension) {
                    found = true;
                    break;
                }
            }

            if(!found) {
                if(ignoreRequiredExtensions) {
                    if(!(flags() & ImporterFlag::Quiet))
                        Warning{} << "Trade::GltfImporter::openData(): required extension" << extension << "not supported, ignoring";
                } else {
                    Error{} << "Trade::GltfImporter::openData(): required extension" << extension << "not supported, enable ignoreRequiredExtensions to ignore";
                    return;
                }
            }
        }
    }

    /* Populate arrays of glTF objects */
    const auto populate = [](Utility::Json& gltf, Containers::Array<Containers::Reference<const Utility::JsonTokenData>>& out, Containers::StringView key, const char* item) {
        if(const Utility::JsonIterator gltfObjects = gltf.root().find(key)) {
            if(!gltf.parseArray(*gltfObjects)) {
                Error{} << "Trade::GltfImporter::openData(): invalid" << key << "property";
                return false;
            }
            for(Utility::JsonArrayItem const gltfObject: gltfObjects->asArray()) {
                if(!gltf.parseObject(gltfObject)) {
                    Error{} << "Trade::GltfImporter::openData(): invalid" << item << gltfObject.index();
                    return false;
                }

                arrayAppend(out, gltfObject.value().token());
            }
        }

        return true;
    };
    const auto populateWithName = [](Utility::Json& gltf, Utility::JsonToken root, Containers::Array<Containers::Pair<Containers::Reference<const Utility::JsonTokenData>, Containers::StringView>>& out, Containers::StringView key, const char* item) {
        if(const Utility::JsonIterator gltfObjects = root.find(key)) {
            if(!gltf.parseArray(*gltfObjects)) {
                Error{} << "Trade::GltfImporter::openData(): invalid" << key << "property";
                return false;
            }
            for(Utility::JsonArrayItem gltfObject: gltfObjects->asArray()) {
                if(!gltf.parseObject(gltfObject)) {
                    Error{} << "Trade::GltfImporter::openData(): invalid" << item << gltfObject.index();
                    return false;
                }

                const Utility::JsonIterator gltfName = gltfObject.value().find("name"_s);
                if(gltfName && !gltf.parseString(*gltfName)) {
                    Error{} << "Trade::GltfImporter::openData(): invalid" << item << gltfObject.index() << "name property";
                    return false;
                }

                arrayAppend(out, InPlaceInit, gltfObject.value().token(), gltfName ? gltfName->asString() : Containers::StringView{});
            }
        }

        return true;
    };
    const auto populateExtensionWithName = [](Utility::Json& gltf, Utility::JsonToken extension, Containers::Array<Containers::Pair<Containers::Reference<const Utility::JsonTokenData>, Containers::StringView>>& out, Containers::StringView key, const char* item) {
        if(!gltf.parseObject(extension)) {
            Error{} << "Trade::GltfImporter::openData(): invalid" << extension.parent()->asString() << "extension";
            return false;
        }

        if(const Utility::JsonIterator gltfObjects = extension.find(key)) {
            if(!gltf.parseArray(*gltfObjects)) {
                Error{} << "Trade::GltfImporter::openData(): invalid" << extension.parent()->asString() << key << "property";
                return false;
            }
            for(Utility::JsonArrayItem gltfObject: gltfObjects->asArray()) {
                if(!gltf.parseObject(gltfObject)) {
                    Error{} << "Trade::GltfImporter::openData(): invalid" << extension.parent()->asString() << item << gltfObject.index();
                    return false;
                }

                const Utility::JsonIterator gltfName = gltfObject.value().find("name"_s);
                if(gltfName && !gltf.parseString(*gltfName)) {
                    Error{} << "Trade::GltfImporter::openData(): invalid" << extension.parent()->asString() << item << gltfObject.index() << "name property";
                    return false;
                }

                arrayAppend(out, InPlaceInit, gltfObject.value().token(), gltfName ? gltfName->asString() : Containers::StringView{});
            }
        }

        return true;
    };
    if(!populate(*gltf, _d->gltfBuffers, "buffers"_s, "buffer") ||
       !populate(*gltf, _d->gltfBufferViews, "bufferViews"_s, "buffer view") ||
       !populate(*gltf, _d->gltfAccessors, "accessors"_s, "accessor") ||
       !populate(*gltf, _d->gltfSamplers, "samplers"_s, "sampler") ||
       !populateWithName(*gltf, gltf->root(), _d->gltfNodes, "nodes"_s, "node") ||
       !populateWithName(*gltf, gltf->root(), _d->gltfMeshes, "meshes"_s, "mesh") ||
       /* Mesh primitives done below */
       !populateWithName(*gltf, gltf->root(), _d->gltfCameras, "cameras"_s, "camera") ||
       /* Light taken from an extension, done below */
       !populateWithName(*gltf, gltf->root(), _d->gltfAnimations, "animations"_s, "animation") ||
       !populateWithName(*gltf, gltf->root(), _d->gltfSkins, "skins"_s, "skin") ||
       !populateWithName(*gltf, gltf->root(), _d->gltfImages, "images"_s, "image") ||
       !populateWithName(*gltf, gltf->root(), _d->gltfTextures, "textures"_s, "texture") ||
       !populateWithName(*gltf, gltf->root(), _d->gltfMaterials, "materials"_s, "material") ||
       !populateWithName(*gltf, gltf->root(), _d->gltfScenes, "scenes"_s, "scene")
    )
        return;

    /* Extensions */
    if(const Utility::JsonIterator gltfExtensions = gltf->root().find("extensions"_s)) {
        if(!gltf->parseObject(*gltfExtensions)) {
            Error{} << "Trade::GltfImporter::openData(): invalid extensions property";
            return;
        }

        /* Lights */
        if(const Utility::JsonIterator gltfKhrLightsPunctual = gltfExtensions->find("KHR_lights_punctual"_s)) {
            /* This doesn't check that the lights property is actually there
               (which is required by the spec), but that's fine -- if it'd ever
               get to core glTF, it would become optional */
            if(!populateExtensionWithName(*gltf, *gltfKhrLightsPunctual, _d->gltfLights, "lights"_s, "light"))
                return;
        }
    }

    /* Find cycles in node tree. The Tortoise and Hare algorithm relies on
       elements of the graph having a single outgoing edge, which means we have
       to build parent links first. During that process we check that nodes
       don't have multiple parents. */
    {
        /* Mark all nodes as unreferenced (-2) first -- if a node isn't
           referenced from any scene nodes or node children array, it'll stay
           that way */
        /** @todo this could be eventually used to compile a "leftovers" scene
            out of unreferenced nodes */
        Containers::Array<Int> nodeParents{DirectInit, _d->gltfNodes.size(), -2};

        /* Mark all nodes referenced by a scene as root nodes (-1) */
        for(std::size_t i = 0; i != _d->gltfScenes.size(); ++i) {
            const Utility::JsonIterator gltfSceneNodes = Utility::JsonToken{*gltf, _d->gltfScenes[i].first()}.find("nodes"_s);
            if(!gltfSceneNodes)
                continue;

            const Containers::Optional<Containers::StridedArrayView1D<const UnsignedInt>> sceneNodes = gltf->parseUnsignedIntArray(*gltfSceneNodes);
            if(!sceneNodes) {
                Error{} << "Trade::GltfImporter::openData(): invalid nodes property of scene" << i;
                return;
            }

            for(const UnsignedInt node: *sceneNodes) {
                if(node >= _d->gltfNodes.size()) {
                    Error{} << "Trade::GltfImporter::openData(): node index" << node << "in scene" << i << "out of range for" << _d->gltfNodes.size() << "nodes";
                    return;
                }

                /* In this case it's fine if a node is referenced by multiple
                   scenes (and it's allowed by glTF) */
                nodeParents[node] = -1;
            }
        }

        /* Go through the node hierarchy and mark nested children, discovering
           potential conflicting parent nodes */
        for(std::size_t i = 0; i != _d->gltfNodes.size(); ++i) {
            const Utility::JsonIterator gltfNodeChildren = Utility::JsonToken{*gltf, _d->gltfNodes[i].first()}.find("children"_s);
            if(!gltfNodeChildren)
                continue;

            const Containers::Optional<Containers::StridedArrayView1D<const UnsignedInt>> nodeChildren = gltf->parseUnsignedIntArray(*gltfNodeChildren);
            if(!nodeChildren) {
                Error{} << "Trade::GltfImporter::openData(): invalid children property of node" << i;
                return;
            }

            for(const UnsignedInt child: *nodeChildren) {
                if(child >= _d->gltfNodes.size()) {
                    Error{} << "Trade::GltfImporter::openData(): child index" << child << "in node" << i << "out of range for" << _d->gltfNodes.size() << "nodes";
                    return;
                }

                /* If a referenced child already has a parent assigned, it's a
                   cycle */
                if(nodeParents[child] == -1) {
                    Error{} << "Trade::GltfImporter::openData(): node" << child << "is both a root node and a child of node" << i;
                    return;
                } else if(nodeParents[child] != -2) {
                    Error{} << "Trade::GltfImporter::openData(): node" << child << "is a child of both node" << nodeParents[child] << "and node" << i;
                    return;
                }

                nodeParents[child] = i;
            }
        }

        /* Find cycles, Tortoise and Hare */
        for(std::size_t i = 0; i != _d->gltfNodes.size(); ++i) {
            Int p1 = nodeParents[i];
            Int p2 = p1 < 0 ? -1 : nodeParents[p1];

            while(p1 >= 0 && p2 >= 0) {
                if(p1 == p2) {
                    Error{} << "Trade::GltfImporter::openData(): node tree contains cycle starting at node" << i;
                    return;
                }

                p1 = nodeParents[p1];
                p2 = nodeParents[p2] < 0 ? -1 : nodeParents[nodeParents[p2]];
            }
        }
    }

    /* Go through all nodes and collect names of extra properties for custom
       scene fields */
    for(std::size_t i = 0; i != _d->gltfNodes.size(); ++i) {
        const Utility::JsonToken gltfNode{*gltf, _d->gltfNodes[i].first()};
        const Utility::JsonIterator gltfExtras = gltfNode.find("extras"_s);
        /* Silently skip also if extras isn't an object -- the error will be
           printed when importing the actual scene containing this node */
        if(!gltfExtras || gltfExtras->type() != Utility::JsonToken::Type::Object)
            continue;
        /* However if the object fails to parse because it has invalid keys
           (i.e., invalid Unicode escapes), fail the whole import. If we
           wouldn't, it'd still print a message to the output which would imply
           an error was silently ignored, which is not any better. */
        if(!gltf->parseObject(*gltfExtras)) {
            Error{} << "Trade::GltfImporter::openData(): invalid node" << i << "extras property";
            return;
        }

        /* The process is recursive so it has to be an external function */
        const Utility::ConfigurationGroup* customSceneFieldTypeConfiguration = configuration().group("customSceneFieldTypes");
        for(const Utility::JsonObjectItem gltfExtra: gltfExtras->asObject()) {
            if(!discoverSceneExtraFields(*gltf, _d->sceneFieldsForName, _d->sceneFieldNamesTypesFlags, customSceneFieldTypeConfiguration, i, gltfExtra.key(), gltfExtra.value()))
                return;
        }
    }

    /* Treat meshes with multiple primitives as separate meshes. Each mesh gets
       duplicated as many times as is the size of the primitives array.
       Conservatively reserve for exactly one primitive per mesh, as that's the
       most common case. */
    arrayReserve(_d->gltfMeshPrimitiveMap, _d->gltfMeshes.size());
    _d->meshSizeOffsets = Containers::Array<std::size_t>{_d->gltfMeshes.size() + 1};
    _d->meshSizeOffsets[0] = 0;
    for(std::size_t i = 0; i != _d->gltfMeshes.size(); ++i) {
        const Utility::JsonIterator gltfMeshPrimitives = Utility::JsonToken{*gltf, *_d->gltfMeshes[i].first()}.find("primitives"_s);
        if(!gltfMeshPrimitives || !gltf->parseArray(*gltfMeshPrimitives)) {
            Error{} << "Trade::GltfImporter::openData(): missing or invalid primitives property in mesh" << i;
            return;
        }

        /* Yes, this isn't array item count but rather a size of the whole
           subtree, but that's fine as we only check it's non-empty */
        if(gltfMeshPrimitives->childCount() == 0) {
            Error{} << "Trade::GltfImporter::openData(): mesh" << i << "has no primitives";
            return;
        }

        for(Utility::JsonArrayItem gltfPrimitive: gltfMeshPrimitives->asArray()) {
            if(!gltf->parseObject(gltfPrimitive.value())) {
                Error{} << "Trade::GltfImporter::openData(): invalid mesh" << i << "primitive" << gltfPrimitive.index();
                return;
            }

            arrayAppend(_d->gltfMeshPrimitiveMap, InPlaceInit, i, gltfPrimitive.value().token());
        }

        _d->meshSizeOffsets[i + 1] = _d->gltfMeshPrimitiveMap.size();
    }

    /* Go through all meshes, collect custom attributes and decide about
       implicitly enabling textureCoordinateYFlipInMaterial if it isn't already
       requested from the configuration and there are any texture coordinates
       that need it */
    if(configuration().value<bool>("textureCoordinateYFlipInMaterial"))
        _d->textureCoordinateYFlipInMaterial = true;
    for(std::size_t i = 0; i != _d->gltfMeshPrimitiveMap.size(); ++i) {
        const auto collectCustomAttributesDecideTextureCoordinateYFlip = [this, i](Utility::Json& gltf, Utility::JsonToken gltfAttributes, Int morphTargetId) {
            for(Utility::JsonObjectItem gltfAttribute: gltfAttributes.asObject()) {
                /* Decide about texture coordinate Y flipping if not set
                   already */
                if(gltfAttribute.key().hasPrefix("TEXCOORD_"_s) && isBuiltinNumberedMeshAttribute(gltfAttribute.key())) {
                    if(_d->textureCoordinateYFlipInMaterial)
                        continue;

                    /* Perform a subset of parsing and validation done in
                       doMesh() and parseAccessor(). Not calling
                       parseAccessor() here because it would cause the actual
                       buffers to be loaded and a ton other validation
                       performed, which is undesirable during the initial file
                       opening.

                       On the other hand, for simplicity also not making
                       doMesh() or parseAccessor() assume any of this was
                       already parsed, except for validation of the attributes
                       object in the outer loop, which is guaranteed to be done
                       for all meshes. */

                    if(!gltf.parseUnsignedInt(gltfAttribute.value())) {
                        Error e;
                        e << "Trade::GltfImporter::openData(): invalid attribute" << gltfAttribute.key();
                        if(morphTargetId != -1)
                            e << "in morph target" << morphTargetId;
                        e << "in mesh" << _d->gltfMeshPrimitiveMap[i].first();
                        return false;
                    }
                    if(gltfAttribute.value().asUnsignedInt() >= _d->gltfAccessors.size()) {
                        Error{} << "Trade::GltfImporter::openData(): accessor index" << gltfAttribute.value().asUnsignedInt() << "out of range for" << _d->gltfAccessors.size() << "accessors";
                        return false;
                    }

                    const Utility::JsonToken gltfAccessor{gltf, _d->gltfAccessors[gltfAttribute.value().asUnsignedInt()]};

                    const Utility::JsonIterator gltfAccessorComponentType = gltfAccessor.find("componentType"_s);
                    if(!gltfAccessorComponentType || !gltf.parseUnsignedInt(*gltfAccessorComponentType)) {
                        Error{} << "Trade::GltfImporter::openData(): accessor" << gltfAttribute.value().asUnsignedInt() << "has missing or invalid componentType property";
                        return false;
                    }

                    /* Normalized is optional, defaulting to false */
                    const Utility::JsonIterator gltfAccessorNormalized = gltfAccessor.find("normalized"_s);
                    if(gltfAccessorNormalized && !gltf.parseBool(*gltfAccessorNormalized)) {
                        Error{} << "Trade::GltfImporter::openData(): accessor" << gltfAttribute.value().asUnsignedInt() << "has invalid normalized property";
                        return false;
                    }

                    const UnsignedInt accessorComponentType = gltfAccessorComponentType->asUnsignedInt();
                    const bool normalized = gltfAccessorNormalized && gltfAccessorNormalized->asBool();
                    if(accessorComponentType == Implementation::GltfTypeByte ||
                       accessorComponentType == Implementation::GltfTypeShort ||
                      (accessorComponentType == Implementation::GltfTypeUnsignedByte && !normalized) ||
                      (accessorComponentType == Implementation::GltfTypeUnsignedShort && !normalized))
                    {
                        Debug{} << "Trade::GltfImporter::openData(): file contains non-normalized texture coordinates, implicitly enabling textureCoordinateYFlipInMaterial";
                        _d->textureCoordinateYFlipInMaterial = true;
                    }
                }

                /* Add the attribute to custom if not there already. Do it for
                   all builtin attributes as well, as those may still get
                   imported as custom if they have a strange vertex format. */
                if(_d->meshAttributesForName.emplace(gltfAttribute.key(),
                    meshAttributeCustom(_d->meshAttributeNames.size())).second
                )
                    arrayAppend(_d->meshAttributeNames, gltfAttribute.key());

                /* The spec says that all user-defined attributes must start
                   with an underscore. We don't really care and just print a
                   warning. */
                /** @todo make this fail if strict mode is enabled? */
                if(!(flags() & ImporterFlag::Quiet) && !isBuiltinMeshAttribute(configuration(), gltfAttribute.key()) && !gltfAttribute.key().hasPrefix("_"_s))
                    Warning{} << "Trade::GltfImporter::openData(): unknown attribute" << gltfAttribute.key() << Debug::nospace << ", importing as custom attribute";
            }

            return true;
        };

        const Utility::JsonToken gltfPrimitive{*gltf, _d->gltfMeshPrimitiveMap[i].second()};

        /* The glTF spec requires a primitive to define an attribute property
           with at least one attribute, but we're fine without here. Stricter
           checks, if any, are done in doMesh(). */
        if(const Utility::JsonIterator gltfAttributes = gltfPrimitive.find("attributes"_s)) {
            if(!gltf->parseObject(*gltfAttributes)) {
                Error{} << "Trade::GltfImporter::openData(): invalid primitive attributes property in mesh" << _d->gltfMeshPrimitiveMap[i].first();
                return;
            }

            if(!collectCustomAttributesDecideTextureCoordinateYFlip(*gltf, *gltfAttributes, -1))
                return;
        }

        /* Go through any morph targets, collecting custom attributes and
           deciding about texture coordinate Y-flip for those as well */
        if(const Utility::JsonIterator gltfTargets = gltfPrimitive.find("targets"_s)) {
            if(!gltf->parseArray(*gltfTargets)) {
                Error{} << "Trade::GltfImporter::openData(): invalid primitive targets property in mesh" << _d->gltfMeshPrimitiveMap[i].first();
                return;
            }

            for(Utility::JsonArrayItem gltfTarget: gltfTargets->asArray()) {
                if(!gltf->parseObject(gltfTarget)) {
                    Error{} << "Trade::GltfImporter::openData(): invalid morph target" << gltfTarget.index() << "in mesh" << _d->gltfMeshPrimitiveMap[i].first();
                    return;
                }

                if(!collectCustomAttributesDecideTextureCoordinateYFlip(*gltf, gltfTarget.value(), gltfTarget.index()))
                    return;
            }
        }
    }

    /* Discover 2D array images -- if any KHR_texture_ktx texture extension
       has a layer property, given image is 2D array. Otherwise it's 2D. To
       make the logic more robust, the same image can't be referenced as both
       2D and 2D array however -- so we go through all images and check. */
    {
        /* 0 is unknown, 2 is 2D, 3 is 2D array. In case of 2D array, second
           item is the index into _d->uniqueTextures which references the first
           texture that referenced given image. */
        Containers::Array<Containers::Pair<UnsignedInt, UnsignedInt>> imageDimensionsAssociatedArrayTextures{ValueInit, _d->gltfImages.size()};
        _d->uniqueTextureForGltfTexture = Containers::Array<UnsignedInt>{NoInit, _d->gltfTextures.size()};
        for(UnsignedInt i = 0; i != _d->gltfTextures.size(); ++i) {
            const Utility::JsonToken gltfTexture{*gltf, _d->gltfTextures[i].first()};

            Utility::JsonIterator gltfSource;
            bool is2DArrayLayer = false;

            /* If the experimentalKhrTextureKtx option is disabled, there's
               currently no way for 3D images to exist, so we can safely assume
               all images are 2D and don't need to check any image sources. */
            if(configuration().value<bool>("experimentalKhrTextureKtx")) {
                /* As in doTexture(), pick the first available image, assuming
                   that extension order indicates a preference... */
                if(const Utility::JsonIterator gltfTextureExtensions = gltfTexture.find("extensions"_s)) {
                    if(!gltf->parseObject(*gltfTextureExtensions)) {
                        Error{} << "Trade::GltfImporter::openData(): invalid extensions property in texture" << i;
                        return;
                    }

                    /* Pick the first extension we understand. If
                       KHR_texture_ktx isn't the first, it won't be picked. */
                    for(const Utility::JsonObjectItem j: gltfTextureExtensions->asObject()) {
                        const Containers::StringView extensionName = j.key();
                        const bool isKhrTextureKtx = extensionName == "KHR_texture_ktx"_s;

                        /* Skip unrecognized extensions -- those could be 1D or
                           3D for all we know */
                        if(!isKhrTextureKtx && !isRecognized2DTextureExtension(extensionName))
                            continue;

                        if(!gltf->parseObject(j.value())) {
                            Error{} << "Trade::GltfImporter::openData(): invalid" << extensionName << "extension in texture" << i;
                            return;
                        }

                        /* Retrieve the source here already and not in common
                           code below so we can include the extension name in
                           the error message. For the image index bounds check
                           it's not as important as the offending extension can
                           be located from the reported image ID. */
                        gltfSource = j.value().find("source"_s);
                        if(!gltfSource || !gltf->parseUnsignedInt(*gltfSource)) {
                            Error{} << "Trade::GltfImporter::openData(): missing or invalid" << extensionName << "source property in texture" << i;
                            return;
                        }

                        if(isKhrTextureKtx && j.value().find("layer"))
                            is2DArrayLayer = true;

                        break;
                    }
                }

                /* ... and the core image is a fallback if everything else
                   fails */
                if(!gltfSource) {
                    gltfSource = gltfTexture.find("source"_s);
                    if(!gltfSource || !gltf->parseUnsignedInt(*gltfSource)) {
                        Error{} << "Trade::GltfImporter::openData(): missing or invalid source property in texture" << i;
                        return;
                    }
                }

                if(gltfSource->asUnsignedInt() >= _d->gltfImages.size()) {
                    Error{} << "Trade::GltfImporter::openData(): index" << gltfSource->asUnsignedInt() << "in texture" << i << "out of range for" << _d->gltfImages.size() << "images";
                    return;
                }
            }

            /* If the referenced image is a 2D array layer, remember the first
               texture that references it, and ignore all other textures that
               reference the same images. */
            /** @todo This discards the other textures even if they have
                different sampler properties -- the deduplication should take
                those into account as well, basically parsing the full texture
                data from the start :/ */
            if(is2DArrayLayer) {
                CORRADE_INTERNAL_ASSERT(gltfSource);
                Containers::Pair<UnsignedInt, UnsignedInt>& imageDimensionAssociatedArrayTexture = imageDimensionsAssociatedArrayTextures[gltfSource->asUnsignedInt()];
                if(imageDimensionAssociatedArrayTexture.first() == 2) {
                    Error{} << "Trade::GltfImporter::openData(): texture" << i << "references image" << gltfSource->asUnsignedInt() << "as a 2D array layer but an earlier texture referenced it as 2D";
                    return;
                } else if(imageDimensionAssociatedArrayTexture.first() == 0) {
                    imageDimensionAssociatedArrayTexture = {3, UnsignedInt(_d->uniqueTextures.size())};
                    _d->uniqueTextureForGltfTexture[i] = _d->uniqueTextures.size();
                    arrayAppend(_d->uniqueTextures, i);
                } else {
                    CORRADE_INTERNAL_ASSERT(imageDimensionAssociatedArrayTexture.first() == 3);
                    _d->uniqueTextureForGltfTexture[i] = imageDimensionAssociatedArrayTexture.second();
                }

            /* Otherwise it's 2D and each texture is unique. Check & update
               source image dimensionality, if we have it. */
            } else {
                if(gltfSource) {
                    UnsignedInt& imageDimension = imageDimensionsAssociatedArrayTextures[gltfSource->asUnsignedInt()].first();
                    if(imageDimension == 3) {
                        Error{} << "Trade::GltfImporter::openData(): texture" << i << "references image" << gltfSource->asUnsignedInt() << "as 2D but an earlier texture referenced it as a 2D array layer";
                        return;
                    } else if(imageDimension == 0) {
                        imageDimension = 2;
                    } else CORRADE_INTERNAL_ASSERT(imageDimension == 2);
                }

                _d->uniqueTextureForGltfTexture[i] = _d->uniqueTextures.size();
                arrayAppend(_d->uniqueTextures, i);
            }
        }

        /* Create a partitioned image map -- first 2D images, then 2D array.
           Images that were unreferenced (having 0 for the dimensions) are
           assumed to be 2D as well. */
        _d->imagesByDimension = Containers::Array<UnsignedInt>{NoInit, _d->gltfImages.size()};
        _d->imageByDimensionForGltfImage = Containers::Array<UnsignedInt>{NoInit, _d->gltfImages.size()};
        std::size_t offset = 0;
        for(std::size_t i = 0; i != _d->gltfImages.size(); ++i) {
            if(imageDimensionsAssociatedArrayTextures[i].first() == 0 ||
               imageDimensionsAssociatedArrayTextures[i].first() == 2) {
                _d->imageByDimensionForGltfImage[i] = offset;
                _d->imagesByDimension[offset++] = i;
            }
        }
        _d->image2DCount = offset;
        for(std::size_t i = 0; i != _d->gltfImages.size(); ++i) {
            if(imageDimensionsAssociatedArrayTextures[i].first() == 3) {
                _d->imageByDimensionForGltfImage[i] = offset;
                _d->imagesByDimension[offset++] = i;
            }
        }
        CORRADE_INTERNAL_ASSERT(offset == _d->gltfImages.size());
    }

    /* Parse default scene, as we can't fail in doDefaultScene() */
    if(const Utility::JsonIterator gltfScene = gltf->root().find("scene"_s)) {
        if(!gltf->parseUnsignedInt(*gltfScene)) {
            Error{} << "Trade::GltfImporter::openData(): invalid scene property";
            return;
        }
        if(gltfScene->asUnsignedInt() >= _d->gltfScenes.size()) {
            Error{} << "Trade::GltfImporter::openData(): scene index" << gltfScene->asUnsignedInt() << "out of range for" << _d->gltfScenes.size() << "scenes";
            return;
        }
    }

    /* All good, save the parsed state */
    _d->gltf = Utility::move(gltf);

    /* Allocate storage for parsed buffers, buffer views and accessors */
    _d->buffers = Containers::Array<Containers::Optional<Containers::Array<char>>>{_d->gltfBuffers.size()};
    _d->bufferViews = Containers::Array<Containers::Optional<BufferView>>{_d->gltfBufferViews.size()};
    _d->accessors = Containers::Array<Containers::Optional<Accessor>>{_d->gltfAccessors.size()};
    _d->samplers = Containers::Array<Containers::Optional<Sampler>>{_d->gltfSamplers.size()};

    /* Name maps are lazy-loaded because these might not be needed every time */
}

UnsignedInt GltfImporter::doAnimationCount() const {
    /* If the animations are merged, there's at most one */
    if(configuration().value<bool>("mergeAnimationClips"))
        return _d->gltfAnimations.isEmpty() ? 0 : 1;

    return _d->gltfAnimations.size();
}

Int GltfImporter::doAnimationForName(const Containers::StringView name) {
    /* If the animations are merged, don't report any names */
    if(configuration().value<bool>("mergeAnimationClips"))
        return -1;

    if(!_d->animationsForName) {
        _d->animationsForName.emplace();
        _d->animationsForName->reserve(_d->gltfAnimations.size());
        for(std::size_t i = 0; i != _d->gltfAnimations.size(); ++i)
            if(const Containers::StringView n = _d->gltfAnimations[i].second())
                _d->animationsForName->emplace(n, i);
    }

    const auto found = _d->animationsForName->find(name);
    return found == _d->animationsForName->end() ? -1 : found->second;
}

Containers::String GltfImporter::doAnimationName(const UnsignedInt id) {
    /* If the animations are merged, don't report any names */
    if(configuration().value<bool>("mergeAnimationClips"))
        return {};
    return _d->gltfAnimations[id].second();
}

namespace {

template<class V> void postprocessSplineTrack(const UnsignedInt timeTrackUsed, const Containers::ArrayView<const Float> keys, const Containers::ArrayView<Math::CubicHermite<V>> values) {
    /* Already processed, don't do that again */
    if(timeTrackUsed != ~UnsignedInt{})
        return;

    CORRADE_INTERNAL_ASSERT(keys.size() == values.size());
    if(keys.size() < 2)
        return;

    /* Convert the `a` values to `n` and the `b` values to `m` as described in
       https://github.com/KhronosGroup/glTF/tree/master/specification/2.0#appendix-c-spline-interpolation
       Unfortunately I was not able to find any concrete name for this, so it's
       not part of the CubicHermite implementation but is kept here locally. */
    for(std::size_t i = 0; i < keys.size() - 1; ++i) {
        const Float timeDifference = keys[i + 1] - keys[i];
        values[i].outTangent() *= timeDifference;
        values[i + 1].inTangent() *= timeDifference;
    }
}

}

Containers::Optional<AnimationData> GltfImporter::doAnimation(UnsignedInt id) {
    /* Import either a single animation or all of them together. At the moment,
       Blender doesn't really support cinematic animations (affecting multiple
       objects): https://blender.stackexchange.com/q/5689. And since
       https://github.com/KhronosGroup/glTF-Blender-Exporter/pull/166, these
       are exported as a set of object-specific clips, which may not be wanted,
       so we give the users an option to merge them all together. */
    const std::size_t animationBegin =
        configuration().value<bool>("mergeAnimationClips") ? 0 : id;
    const std::size_t animationEnd =
        configuration().value<bool>("mergeAnimationClips") ? _d->gltfAnimations.size() : id + 1;

    const Containers::StridedArrayView1D<Containers::Reference<const Utility::JsonTokenData>> gltfAnimations = stridedArrayView(_d->gltfAnimations.slice(animationBegin, animationEnd)).slice(&decltype(_d->gltfAnimations)::Type::first);

    /* Parsed data for samplers in each processed animation. Stored in a
       contiguous array, data for sampler `j` of animation `i` is at
       `animationSamplerData[animationSamplerDataOffsets[i] + j]`. */
    struct AnimationSamplerData {
        UnsignedInt input;
        UnsignedInt output;
        Animation::Interpolation interpolation;
    };
    Containers::Array<AnimationSamplerData> animationSamplerData;
    Containers::Array<UnsignedInt> animationSamplerDataOffsets{NoInit, gltfAnimations.size() + 1};
    /* First gather the input and output data ranges. Key is unique accessor
       ID so we don't duplicate shared data, value is offset in the output data
       and ID of the corresponding key track in case given track is a spline
       interpolation. The time track ID is initialized to ~UnsignedInt{} and
       will be used later to check that a spline track was not used with more
       than one time track, as it needs to be postprocessed for given time
       track. */
    struct SamplerData {
        std::size_t outputOffset;
        UnsignedInt timeTrack;
    };
    std::unordered_map<UnsignedInt, SamplerData> samplerData;
    std::size_t dataSize = 0;
    for(std::size_t i = 0; i != gltfAnimations.size(); ++i) {
        const Utility::JsonToken gltfAnimation{*_d->gltf, gltfAnimations[i]};
        const Utility::JsonIterator gltfAnimationSamplers = gltfAnimation.find("samplers"_s);
        if(!gltfAnimationSamplers || !_d->gltf->parseArray(*gltfAnimationSamplers)) {
            Error{} << "Trade::GltfImporter::animation(): missing or invalid samplers property";
            return {};
        }

        /* Save offset at which samplers for this animation will be stored */
        animationSamplerDataOffsets[i] = animationSamplerData.size();

        for(const Utility::JsonArrayItem gltfAnimationSampler: gltfAnimationSamplers->asArray()) {
            if(!_d->gltf->parseObject(gltfAnimationSampler)) {
                Error{} << "Trade::GltfImporter::animation(): invalid sampler" << gltfAnimationSampler.index();
                return {};
            }

            const Utility::JsonIterator gltfAnimationSamplerInput = gltfAnimationSampler.value().find("input"_s);
            if(!gltfAnimationSamplerInput || !_d->gltf->parseUnsignedInt(*gltfAnimationSamplerInput)) {
                Error{} << "Trade::GltfImporter::animation(): missing or invalid sampler" << gltfAnimationSampler.index() << "input property";
                return {};
            }

            const Utility::JsonIterator gltfAnimationSamplerOutput = gltfAnimationSampler.value().find("output"_s);
            if(!gltfAnimationSamplerOutput || !_d->gltf->parseUnsignedInt(*gltfAnimationSamplerOutput)) {
                Error{} << "Trade::GltfImporter::animation(): missing or invalid sampler" << gltfAnimationSampler.index() << "output property";
                return {};
            }

            /* Interpolation is optional, LINEAR if not present */
            const Utility::JsonIterator gltfAnimationSamplerInterpolation = gltfAnimationSampler.value().find("interpolation"_s);
            if(gltfAnimationSamplerInterpolation &&  !_d->gltf->parseString(*gltfAnimationSamplerInterpolation)) {
                Error{} << "Trade::GltfImporter::animation(): invalid sampler" << gltfAnimationSampler.index() << "interpolation property";
                return {};
            }
            const Containers::StringView interpolationString = gltfAnimationSamplerInterpolation ? gltfAnimationSamplerInterpolation->asString() : "LINEAR"_s;
            Animation::Interpolation interpolation;
            if(interpolationString == "LINEAR"_s)
                interpolation = Animation::Interpolation::Linear;
            else if(interpolationString == "STEP"_s)
                interpolation = Animation::Interpolation::Constant;
            else if(interpolationString == "CUBICSPLINE"_s)
                interpolation = Animation::Interpolation::Spline;
            else {
                Error{} << "Trade::GltfImporter::animation(): unrecognized sampler" << gltfAnimationSampler.index() << "interpolation" << interpolationString;
                return {};
            }

            /** @todo handle alignment once we do more than just four-byte types */

            /* If the input view is not yet present in the output data buffer,
               add it */
            if(samplerData.find(gltfAnimationSamplerInput->asUnsignedInt()) == samplerData.end()) {
                const Containers::Optional<Accessor> accessor = parseAccessor("Trade::GltfImporter::animation():", gltfAnimationSamplerInput->asUnsignedInt());
                if(!accessor)
                    return {};

                /* There's no technical reason this couldn't work, it's just
                   that I don't see any practical use case that would warrant
                   the extra testing effort, so just fail for now */
                if(accessor->bufferView == ~UnsignedInt{}) {
                    Error{} << "Trade::GltfImporter::animation(): input accessor" << gltfAnimationSamplerInput->asUnsignedInt() << "has no buffer view, which is unsupported";
                    return {};
                }
                if(accessor->sparseValues.data()) {
                    Error{} << "Trade::GltfImporter::animation(): input accessor" << gltfAnimationSamplerInput->asUnsignedInt() << "is using sparse storage, which is unsupported";
                    return {};
                }

                samplerData.emplace(gltfAnimationSamplerInput->asUnsignedInt(), SamplerData{dataSize, ~UnsignedInt{}});
                dataSize += accessor->data.size()[0]*accessor->data.size()[1];
            }

            /* If the output view is not yet present in the output data buffer,
               add it */
            if(samplerData.find(gltfAnimationSamplerOutput->asUnsignedInt()) == samplerData.end()) {
                const Containers::Optional<Accessor> accessor = parseAccessor("Trade::GltfImporter::animation():", gltfAnimationSamplerOutput->asUnsignedInt());
                if(!accessor)
                    return {};

                /* Same as above */
                if(accessor->bufferView == ~UnsignedInt{}) {
                    Error{} << "Trade::GltfImporter::animation(): output accessor" << gltfAnimationSamplerOutput->asUnsignedInt() << "has no buffer view, which is unsupported";
                    return {};
                }
                if(accessor->sparseValues.data()) {
                    Error{} << "Trade::GltfImporter::animation(): output accessor" << gltfAnimationSamplerOutput->asUnsignedInt() << "is using sparse storage, which is unsupported";
                    return {};
                }

                samplerData.emplace(gltfAnimationSamplerOutput->asUnsignedInt(), SamplerData{dataSize, ~UnsignedInt{}});
                dataSize += accessor->data.size()[0]*accessor->data.size()[1];
            }

            arrayAppend(animationSamplerData, InPlaceInit,
                gltfAnimationSamplerInput->asUnsignedInt(),
                gltfAnimationSamplerOutput->asUnsignedInt(),
                interpolation);
        }
    }

    /* Save final size of animation samplers so we can unconditionally use
       `animationSamplerDataOffsets[i + 1] -  animationSamplerDataOffsets[i]`
       to get sampler count for animation `i` */
    animationSamplerDataOffsets[gltfAnimations.size()] = animationSamplerData.size();

    /* Populate the data array */
    /**
     * @todo Once memory-mapped files are supported, this can all go away
     *      except when spline tracks are present -- in that case we need to
     *      postprocess them and can't just use the memory directly.
     */
    Containers::Array<char> data{dataSize};
    for(const std::pair<const UnsignedInt, SamplerData>& view: samplerData) {
        /* The accessor should be already parsed from above, so just retrieve
           its view instead of going through parseAccessor() again */
        const Containers::StridedArrayView2D<const char> src =
            _d->accessors[view.first]->data;
        const Containers::StridedArrayView2D<char> dst{
            data.exceptPrefix(view.second.outputOffset), src.size()};
        Utility::copy(src, dst);
    }

    /* Calculate total track count. If merging all animations together, this is
       the sum of all clip track counts. */
    std::size_t trackCount = 0;
    for(const Utility::JsonTokenData& gltfAnimationData: gltfAnimations) {
        const Utility::JsonToken gltfAnimation{*_d->gltf, gltfAnimationData};
        const Utility::JsonIterator gltfAnimationChannels = gltfAnimation.find("channels"_s);
        if(!gltfAnimationChannels || !_d->gltf->parseArray(*gltfAnimationChannels)) {
            Error{} << "Trade::GltfImporter::animation(): missing or invalid channels property";
            return {};
        }

        for(const Utility::JsonArrayItem gltfAnimationChannel: gltfAnimationChannels->asArray()) {
            if(!_d->gltf->parseObject(gltfAnimationChannel)) {
                Error{} << "Trade::GltfImporter::animation(): invalid channel" << gltfAnimationChannel.index();
                return {};
            }

            const Utility::JsonIterator gltfAnimationChannelTarget = gltfAnimationChannel.value().find("target"_s);
            if(!gltfAnimationChannelTarget || !_d->gltf->parseObject(*gltfAnimationChannelTarget)) {
                Error{} << "Trade::GltfImporter::animation(): missing or invalid channel" << gltfAnimationChannel.index() << "target property";
                return {};
            }

            /* Skip animations without a target node. See comment below. Also,
               we're not using the node value for anything here, so further
               validation is done below. */
            if(gltfAnimationChannelTarget->find("node"_s))
                ++trackCount;
        }
    }

    /* Import all tracks */
    bool hadToRenormalize = false;
    std::size_t trackId = 0;
    Containers::Array<Trade::AnimationTrackData> tracks{trackCount};
    for(std::size_t i = 0; i != gltfAnimations.size(); ++i) {
        const Utility::JsonToken gltfAnimation{*_d->gltf, gltfAnimations[i]};
        /* Channels parsed and checked above already, so can go directly here */
        for(const Utility::JsonArrayItem gltfAnimationChannel: gltfAnimation["channels"_s].asArray()) {
            const Utility::JsonIterator gltfSampler = gltfAnimationChannel.value().find("sampler"_s);
            if(!gltfSampler || !_d->gltf->parseUnsignedInt(*gltfSampler)) {
                Error{} << "Trade::GltfImporter::animation(): missing or invalid channel" << gltfAnimationChannel.index() << "sampler property";
                return {};
            }
            const std::size_t animationSamplerDataOffset = animationSamplerDataOffsets[i];
            if(gltfSampler->asUnsignedInt() >= animationSamplerDataOffsets[i + 1] - animationSamplerDataOffset) {
                Error{} << "Trade::GltfImporter::animation(): sampler index" << gltfSampler->asUnsignedInt() << "in channel" << gltfAnimationChannel.index() << "out of range for" << animationSamplerDataOffsets[i + 1] - animationSamplerDataOffset << "samplers";
                return {};
            }
            const AnimationSamplerData& sampler = animationSamplerData[animationSamplerDataOffset + gltfSampler->asUnsignedInt()];

            /* Skip animations without a target node. Consistent with
               tinygltf's behavior, currently there are no extensions for
               animating materials or anything else so there's no point in
               importing such animations. */
            const Utility::JsonToken gltfTarget = gltfAnimationChannel.value()["target"_s];
            const Utility::JsonIterator gltfTargetNode = gltfTarget.find("node"_s);
            /** @todo revisit once KHR_animation2 is a thing:
                https://github.com/KhronosGroup/glTF/pull/2033 */
            if(!gltfTargetNode)
                continue;

            if(!_d->gltf->parseUnsignedInt(*gltfTargetNode)) {
                Error{} << "Trade::GltfImporter::animation(): invalid channel" << gltfAnimationChannel.index() << "target node property";
                return {};
            }
            if(gltfTargetNode->asUnsignedInt() >= _d->gltfNodes.size()) {
                Error{} << "Trade::GltfImporter::animation(): target node index" << gltfTargetNode->asUnsignedInt() << "in channel" << gltfAnimationChannel.index() << "out of range for" << _d->gltfNodes.size() << "nodes";
                return {};
            }

            /* Key properties -- always float time. Again, the accessor should
               be already parsed from above, so just retrieve its view instead
               of going through parseAccessor() again. */
            const Accessor& input = *_d->accessors[sampler.input];
            if(input.format != VertexFormat::Float) {
                /* Since we're abusing VertexFormat for all formats, print just
                   the enum value without the prefix to avoid cofusion */
                Error{} << "Trade::GltfImporter::animation(): channel" << gltfAnimationChannel.index() << "time track has unexpected type" << Debug::packed << input.format;
                return {};
            }

            /* View on the key data */
            const auto inputDataFound = samplerData.find(sampler.input);
            CORRADE_INTERNAL_ASSERT(inputDataFound != samplerData.end());
            const auto keys = Containers::arrayCast<Float>(data.sliceSize(
                inputDataFound->second.outputOffset,
                input.data.size()[0]*
                input.data.size()[1]));

            /* Decide on value properties. Again, the accessor should be
               already parsed from above, so just retrieve its view instead of
               going through parseAccessor() again. */
            const Accessor& output = *_d->accessors[sampler.output];
            AnimationTrackTarget target;
            AnimationTrackType type, resultType;
            Containers::StridedArrayView1D<const void> typeErasedValues;
            const auto outputDataFound = samplerData.find(sampler.output);
            CORRADE_INTERNAL_ASSERT(outputDataFound != samplerData.end());
            const auto outputData = data.sliceSize(
                outputDataFound->second.outputOffset,
                output.data.size()[0]*
                output.data.size()[1]);
            UnsignedInt& timeTrackUsed = outputDataFound->second.timeTrack;

            const std::size_t valuesPerKey = sampler.interpolation == Animation::Interpolation::Spline ? 3 : 1;
            if(input.data.size()[0]*valuesPerKey != output.data.size()[0]) {
                Error{} << "Trade::GltfImporter::animation(): channel" << gltfAnimationChannel.index() << "target track size doesn't match time track size, expected" << output.data.size()[0] << "but got" << input.data.size()[0]*valuesPerKey;
                return {};
            }

            const Utility::JsonIterator gltfTargetPath = gltfTarget.find("path"_s);
            if(!gltfTargetPath || !_d->gltf->parseString(*gltfTargetPath)) {
                Error{} << "Trade::GltfImporter::animation(): missing or invalid channel" << gltfAnimationChannel.index() << "target path property";
                return {};
            }

            /* Translation */
            if(gltfTargetPath->asString() == "translation"_s) {
                if(output.format != VertexFormat::Vector3) {
                    /* Since we're abusing VertexFormat for all formats, print
                       just the enum value without the prefix to avoid
                       cofusion */
                    Error{} << "Trade::GltfImporter::animation(): translation track has unexpected type" << Debug::packed << output.format;
                    return {};
                }

                /* Create a view on the value data */
                target = AnimationTrackTarget::Translation3D;
                resultType = AnimationTrackType::Vector3;
                if(sampler.interpolation == Animation::Interpolation::Spline) {
                    /* Postprocess the spline track. This can be done only once
                       for every track -- postprocessSplineTrack() checks
                       that. */
                    const auto values = Containers::arrayCast<CubicHermite3D>(outputData);
                    postprocessSplineTrack(timeTrackUsed, keys, values);

                    type = AnimationTrackType::CubicHermite3D;
                    typeErasedValues = values;
                } else {
                    type = AnimationTrackType::Vector3;
                    typeErasedValues = Containers::arrayCast<Vector3>(outputData);
                }

            /* Rotation */
            } else if(gltfTargetPath->asString() == "rotation"_s) {
                /** @todo rotation can be also normalized (?!) to a vector of 8/16bit (signed?!) integers */

                if(output.format != VertexFormat::Vector4) {
                    /* Since we're abusing VertexFormat for all formats, print
                       just the enum value without the prefix to avoid
                       cofusion */
                    Error{} << "Trade::GltfImporter::animation(): rotation track has unexpected type" << Debug::packed << output.format;
                    return {};
                }

                /* View on the value data */
                target = AnimationTrackTarget::Rotation3D;
                resultType = AnimationTrackType::Quaternion;
                if(sampler.interpolation == Animation::Interpolation::Spline) {
                    /* Postprocess the spline track. This can be done only once
                       for every track -- postprocessSplineTrack() checks
                       that. */
                    const auto values = Containers::arrayCast<CubicHermiteQuaternion>(outputData);
                    postprocessSplineTrack(timeTrackUsed, keys, values);

                    type = AnimationTrackType::CubicHermiteQuaternion;
                    typeErasedValues = values;
                } else {
                    /* Ensure shortest path is always chosen. Not doing this
                       for spline interpolation, there it would cause war and
                       famine. */
                    const auto values = Containers::arrayCast<Quaternion>(outputData);
                    if(configuration().value<bool>("optimizeQuaternionShortestPath")) {
                        Float flip = 1.0f;
                        for(std::size_t j = 0; j + 1 < values.size(); ++j) {
                            if(Math::dot(values[j], values[j + 1]*flip) < 0)
                                flip = -flip;
                            values[j + 1] *= flip;
                        }
                    }

                    /* Normalize the quaternions if not already. Don't attempt
                       to normalize every time to avoid tiny differences, only
                       when the quaternion looks to be off. Again, not doing
                       this for splines as it would cause things to go
                       haywire. */
                    if(configuration().value<bool>("normalizeQuaternions")) {
                        for(auto& quat: values) if(!quat.isNormalized()) {
                            quat = quat.normalized();
                            hadToRenormalize = true;
                        }
                    }

                    type = AnimationTrackType::Quaternion;
                    typeErasedValues = values;
                }

            /* Scale */
            } else if(gltfTargetPath->asString() == "scale"_s) {
                if(output.format != VertexFormat::Vector3) {
                    /* Since we're abusing VertexFormat for all formats, print
                       just the enum value without the prefix to avoid
                       cofusion */
                    Error{} << "Trade::GltfImporter::animation(): scaling track has unexpected type" << Debug::packed << output.format;
                    return {};
                }

                /* View on the value data */
                target = AnimationTrackTarget::Scaling3D;
                resultType = AnimationTrackType::Vector3;
                if(sampler.interpolation == Animation::Interpolation::Spline) {
                    /* Postprocess the spline track. This can be done only once
                       for every track -- postprocessSplineTrack() checks
                       that. */
                    const auto values = Containers::arrayCast<CubicHermite3D>(outputData);
                    postprocessSplineTrack(timeTrackUsed, keys, values);

                    type = AnimationTrackType::CubicHermite3D;
                    typeErasedValues = values;
                } else {
                    type = AnimationTrackType::Vector3;
                    typeErasedValues = Containers::arrayCast<Vector3>(outputData);
                }

            } else {
                Error{} << "Trade::GltfImporter::animation(): unsupported track target" << gltfTargetPath->asString();
                return {};
            }

            /* Splines were postprocessed using the corresponding time track.
               If a spline is not yet marked as postprocessed, mark it.
               Otherwise check that the spline track is always used with the
               same time track. */
            if(sampler.interpolation == Animation::Interpolation::Spline) {
                if(timeTrackUsed == ~UnsignedInt{})
                    timeTrackUsed = sampler.input;
                else if(timeTrackUsed != sampler.input) {
                    Error{} << "Trade::GltfImporter::animation(): spline track is shared with different time tracks, we don't support that, sorry";
                    return {};
                }
            }

            tracks[trackId++] = AnimationTrackData{
                target, gltfTargetNode->asUnsignedInt(),
                type, resultType, keys, typeErasedValues,
                sampler.interpolation, Animation::Extrapolation::Constant};
        }
    }

    if(hadToRenormalize && !(flags() & ImporterFlag::Quiet))
        Warning{} << "Trade::GltfImporter::animation(): quaternions in some rotation tracks were renormalized";

    return AnimationData{Utility::move(data), Utility::move(tracks),
        configuration().value<bool>("mergeAnimationClips") ? nullptr :
        &*_d->gltfAnimations[id].first()};
}

UnsignedInt GltfImporter::doCameraCount() const {
    return _d->gltfCameras.size();
}

Int GltfImporter::doCameraForName(const Containers::StringView name) {
    if(!_d->camerasForName) {
        _d->camerasForName.emplace();
        _d->camerasForName->reserve(_d->gltfCameras.size());
        for(std::size_t i = 0; i != _d->gltfCameras.size(); ++i)
            if(const Containers::StringView n = _d->gltfCameras[i].second())
                _d->camerasForName->emplace(n , i);
    }

    const auto found = _d->camerasForName->find(name);
    return found == _d->camerasForName->end() ? -1 : found->second;
}

Containers::String GltfImporter::doCameraName(const UnsignedInt id) {
    return _d->gltfCameras[id].second();
}

Containers::Optional<CameraData> GltfImporter::doCamera(const UnsignedInt id) {
    const Utility::JsonToken gltfCamera{*_d->gltf, _d->gltfCameras[id].first()};

    const Utility::JsonIterator gltfType = gltfCamera.find("type"_s);
    if(!gltfType || !_d->gltf->parseString(*gltfType)) {
        Error{} << "Trade::GltfImporter::camera(): missing or invalid type property";
        return {};
    }

    /* https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#projection-matrices */

    /* Perspective camera */
    if(gltfType->asString() == "perspective"_s) {
        const Utility::JsonIterator gltfPerspectiveCamera = gltfCamera.find("perspective"_s);
        if(!gltfPerspectiveCamera || !_d->gltf->parseObject(*gltfPerspectiveCamera)) {
            Error{} << "Trade::GltfImporter::camera(): missing or invalid perspective property";
            return {};
        }

        /* Aspect ratio is optional, use 1:1 if not set */
        /** @todo spec says "if not set "aspect ratio of the rendering viewport
            MUST be used", heh, how am I supposed to know that here? */
        const Utility::JsonIterator gltfAspectRatio = gltfPerspectiveCamera->find("aspectRatio"_s);
        if(gltfAspectRatio) {
            if(!_d->gltf->parseFloat(*gltfAspectRatio)) {
                Error{} << "Trade::GltfImporter::camera(): invalid perspective aspectRatio property";
                return {};
            }
            if(gltfAspectRatio->asFloat() <= 0.0f) {
                Error{} << "Trade::GltfImporter::camera(): expected positive perspective aspectRatio, got" << gltfAspectRatio->asFloat();
                return {};
            }
        }

        const Utility::JsonIterator gltfYfov = gltfPerspectiveCamera->find("yfov"_s);
        if(!gltfYfov || !_d->gltf->parseFloat(*gltfYfov)) {
            Error{} << "Trade::GltfImporter::camera(): missing or invalid perspective yfov property";
            return {};
        }
        if(gltfYfov->asFloat() <= 0.0f) {
            Error{} << "Trade::GltfImporter::camera(): expected positive perspective yfov, got" << gltfYfov->asFloat();
            return {};
        }

        const Utility::JsonIterator gltfZnear = gltfPerspectiveCamera->find("znear"_s);
        if(!gltfZnear || !_d->gltf->parseFloat(*gltfZnear)) {
            Error{} << "Trade::GltfImporter::camera(): missing or invalid perspective znear property";
            return {};
        }
        if(gltfZnear->asFloat() <= 0.0f) {
            Error{} << "Trade::GltfImporter::camera(): expected positive perspective znear, got" << gltfZnear->asFloat();
            return {};
        }

        /* Z far is optional, if not set it's infinity (and yes, JSON has no
           way to represent an infinity, FFS) */
        const Utility::JsonIterator gltfZfar = gltfPerspectiveCamera->find("zfar"_s);
        if(gltfZfar) {
            if(!_d->gltf->parseFloat(*gltfZfar)) {
                Error{} << "Trade::GltfImporter::camera(): invalid perspective zfar property";
                return {};
            }
            if(gltfZfar->asFloat() <= gltfZnear->asFloat()) {
                Error{} << "Trade::GltfImporter::camera(): expected perspective zfar larger than znear of" << gltfZnear->asFloat() << Debug::nospace << ", got" << gltfZfar->asFloat();
                return {};
            }
        }

        const Float aspectRatio = gltfAspectRatio ? gltfAspectRatio->asFloat() : 1.0f;
        /* glTF uses vertical FoV and X/Y aspect ratio, so to avoid accidental
           bugs we will directly calculate the near plane size and use that to
           create the camera data (instead of passing it the horizontal FoV) */
        const Vector2 size = 2.0f*gltfZnear->asFloat()*Math::tan(gltfYfov->asFloat()*0.5_radf)*Vector2::xScale(aspectRatio);
        const Float far = gltfZfar ? gltfZfar->asFloat() : Constants::inf();
        return CameraData{CameraType::Perspective3D, size, gltfZnear->asFloat(), far, &gltfCamera.token()};
    }

    /* Orthographic camera */
    if(gltfType->asString() == "orthographic"_s) {
        const Utility::JsonIterator gltfOrthographicCamera = gltfCamera.find("orthographic"_s);
        if(!gltfOrthographicCamera || !_d->gltf->parseObject(*gltfOrthographicCamera)) {
            Error{} << "Trade::GltfImporter::camera(): missing or invalid orthographic property";
            return {};
        }

        const Utility::JsonIterator gltfXmag = gltfOrthographicCamera->find("xmag"_s);
        if(!gltfXmag || !_d->gltf->parseFloat(*gltfXmag)) {
            Error{} << "Trade::GltfImporter::camera(): missing or invalid orthographic xmag property";
            return {};
        }
        if(gltfXmag->asFloat() == 0.0f) {
            Error{} << "Trade::GltfImporter::camera(): expected non-zero orthographic xmag";
            return {};
        }

        const Utility::JsonIterator gltfYmag = gltfOrthographicCamera->find("ymag"_s);
        if(!gltfYmag || !_d->gltf->parseFloat(*gltfYmag)) {
            Error{} << "Trade::GltfImporter::camera(): missing or invalid orthographic ymag property";
            return {};
        }
        if(gltfYmag->asFloat() == 0.0f) {
            Error{} << "Trade::GltfImporter::camera(): expected non-zero orthographic ymag";
            return {};
        }

        const Utility::JsonIterator gltfZnear = gltfOrthographicCamera->find("znear"_s);
        if(!gltfZnear || !_d->gltf->parseFloat(*gltfZnear)) {
            Error{} << "Trade::GltfImporter::camera(): missing or invalid orthographic znear property";
            return {};
        }
        if(gltfZnear->asFloat() < 0.0f) {
            Error{} << "Trade::GltfImporter::camera(): expected non-negative orthographic znear, got" << gltfZnear->asFloat();
            return {};
        }

        const Utility::JsonIterator gltfZfar = gltfOrthographicCamera->find("zfar"_s);
        if(!gltfZfar || !_d->gltf->parseFloat(*gltfZfar)) {
            Error{} << "Trade::GltfImporter::camera(): missing or invalid orthographic zfar property";
            return {};
        }
        if(gltfZfar->asFloat() <= gltfZnear->asFloat()) {
            Error{} << "Trade::GltfImporter::camera(): expected orthographic zfar larger than znear of" << gltfZnear->asFloat() << Debug::nospace << ", got" << gltfZfar->asFloat();
            return {};
        }

        return CameraData{CameraType::Orthographic3D,
            /* glTF uses a "scale" instead of "size", which means we have to
               double */
            Vector2{gltfXmag->asFloat(), gltfYmag->asFloat()}*2.0f,
            gltfZnear->asFloat(), gltfZfar->asFloat(), &gltfCamera.token()};
    }

    Error{} << "Trade::GltfImporter::camera(): unrecognized type" << gltfType->asString();
    return {};
}

UnsignedInt GltfImporter::doLightCount() const {
    return _d->gltfLights.size();
}

Int GltfImporter::doLightForName(const Containers::StringView name) {
    if(!_d->lightsForName) {
        _d->lightsForName.emplace();
        _d->lightsForName->reserve(_d->gltfLights.size());
        for(std::size_t i = 0; i != _d->gltfLights.size(); ++i)
            if(const Containers::StringView n = _d->gltfLights[i].second())
                _d->lightsForName->emplace(n, i);
    }

    const auto found = _d->lightsForName->find(name);
    return found == _d->lightsForName->end() ? -1 : found->second;
}

Containers::String GltfImporter::doLightName(const UnsignedInt id) {
    return _d->gltfLights[id].second();
}

Containers::Optional<LightData> GltfImporter::doLight(const UnsignedInt id) {
    const Utility::JsonToken gltfLight{*_d->gltf, _d->gltfLights[id].first()};

    /* Color is optional, vector of 1.0 is a default if not set */
    Color3 color{1.0f};
    if(const Utility::JsonIterator gltfColor = gltfLight.find("color"_s)) {
        const Containers::Optional<Containers::StridedArrayView1D<const float>> colorArray = _d->gltf->parseFloatArray(*gltfColor, 3);
        if(!colorArray) {
            Error{} << "Trade::GltfImporter::light(): invalid color property";
            return {};
        }

        Utility::copy(*colorArray, color.data());
    }

    /* Intensity is optional, 1.0 is a default if not set */
    const Utility::JsonIterator gltfIntensity = gltfLight.find("intensity"_s);
    if(gltfIntensity && !_d->gltf->parseFloat(*gltfIntensity)) {
        Error{} << "Trade::GltfImporter::light(): invalid intensity property";
        return {};
    }

    /* Range is optional, infinity is a default if not set (and yes, JSON has
       no way to represent an infinity, FFS) */
    const Utility::JsonIterator gltfRange = gltfLight.find("range"_s);
    if(gltfRange) {
        if(!_d->gltf->parseFloat(*gltfRange)) {
            Error{} << "Trade::GltfImporter::light(): invalid range property";
            return {};
        }
        if(gltfRange->asFloat() <= 0.0f) {
            Error{} << "Trade::GltfImporter::light(): expected positive range, got" << gltfRange->asFloat();
            return {};
        }
    }

    const Utility::JsonIterator gltfType = gltfLight.find("type"_s);
    if(!gltfType || !_d->gltf->parseString(*gltfType)) {
        Error{} << "Trade::GltfImporter::light(): missing or invalid type property";
        return {};
    }

    /* Light type */
    LightType type;
    if(gltfType->asString() == "point"_s) {
        type = LightType::Point;
    } else if(gltfType->asString() == "spot"_s) {
        type = LightType::Spot;
    } else if(gltfType->asString() == "directional"_s) {
        type = LightType::Directional;
    } else {
        Error{} << "Trade::GltfImporter::light(): unrecognized type" << gltfType->asString();
        return {};
    }

    /* Spotlight cone angles. In glTF they're specified as half-angles (which
       is also why the limit on outer angle is 90°, not 180°), to avoid
       confusion report a potential error in the original half-angles and
       double the angle only at the end. */
    Rad innerConeAngle{NoInit}, outerConeAngle{NoInit};
    if(type == LightType::Spot) {
        innerConeAngle = 0.0_degf;
        outerConeAngle = 45.0_degf;

        const Utility::JsonIterator gltfSpot = gltfLight.find("spot"_s);
        if(!gltfSpot || !_d->gltf->parseObject(*gltfSpot)) {
            Error{} << "Trade::GltfImporter::light(): missing or invalid spot property";
            return {};
        }

        if(const Utility::JsonIterator gltfInnerConeAngle = gltfSpot->find("innerConeAngle"_s)) {
            const Containers::Optional<Float> angle = _d->gltf->parseFloat(*gltfInnerConeAngle);
            if(!angle) {
                Error{} << "Trade::GltfImporter::light(): invalid spot innerConeAngle property";
                return {};
            }

            innerConeAngle = Rad{*angle};
        }

        if(const Utility::JsonIterator gltfOuterConeAngle = gltfSpot->find("outerConeAngle"_s)) {
            const Containers::Optional<Float> angle = _d->gltf->parseFloat(*gltfOuterConeAngle);
            if(!angle) {
                Error{} << "Trade::GltfImporter::light(): invalid spot outerConeAngle property";
                return {};
            }

            outerConeAngle = Rad{*angle};
        }

        if(innerConeAngle < Rad(0.0_degf) || innerConeAngle >= outerConeAngle || outerConeAngle > Rad(Constants::piHalf())) {
            Error{} << "Trade::GltfImporter::light(): spot inner and outer cone angle" << Deg(innerConeAngle) << "and" << Deg(outerConeAngle) << "out of allowed bounds";
            return {};
        }
    } else innerConeAngle = outerConeAngle = Rad(Constants::pi());

    /* Range should be infinity for directional lights. Because there's no way
       to represent infinity in JSON, directly suggest to remove the range
       property, don't even bother printing the value. */
    if(type == LightType::Directional && gltfRange) {
        Error{} << "Trade::GltfImporter::light(): range can't be defined for a directional light";
        return {};
    }

    /* As said above, glTF uses half-angles, while we have full angles (for
       consistency with existing APIs such as OpenAL cone angles or math intersection routines as well as Blender). */
    return LightData{type, color,
        gltfIntensity ? gltfIntensity->asFloat() : 1.0f,
        gltfRange ? gltfRange->asFloat() : Constants::inf(),
        innerConeAngle*2.0f, outerConeAngle*2.0f, &gltfLight.token()};
}

Int GltfImporter::doDefaultScene() const {
    if(const Utility::JsonIterator gltfScene = _d->gltf->root().find("scene"_s)) {
        /* All checking and parsing was done in doOpenData() already, as this
           function is not allowed to fail */
        return gltfScene->asUnsignedInt();
    }

    /* While https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#scenes
       says that "When scene is undefined, client implementations MAY delay
       rendering until a particular scene is requested.", several official
       sample glTF models (e.g. the AnimatedTriangle) have no "scene" property.
       We could return 0 here if there's at least one scene, but certain use cases may want to rely on (lack of) presence of the scene property, so
       just return -1 if it's not there. Related discussion also at
       https://github.com/KhronosGroup/glTF/issues/815#issuecomment-274286889 */
    return -1;
}

UnsignedInt GltfImporter::doSceneCount() const {
    return _d->gltfScenes.size();
}

Int GltfImporter::doSceneForName(const Containers::StringView name) {
    if(!_d->scenesForName) {
        _d->scenesForName.emplace();
        _d->scenesForName->reserve(_d->gltfScenes.size());
        for(std::size_t i = 0; i != _d->gltfScenes.size(); ++i) {
            if(const Containers::StringView n = _d->gltfScenes[i].second())
                _d->scenesForName->emplace(n, i);
        }
    }

    const auto found = _d->scenesForName->find(name);
    return found == _d->scenesForName->end() ? -1 : found->second;
}

Containers::String GltfImporter::doSceneName(const UnsignedInt id) {
    return _d->gltfScenes[id].second();
}

namespace {

/* Used by doScene() but it's recursive and so it can't be a local lambda */
void parseSceneExtraFields(Utility::Json& gltf, const ImporterFlags flags, const std::unordered_map<Containers::String, SceneField>& sceneFieldsForName, const Containers::ArrayView<const Containers::Triple<Containers::StringView, SceneFieldType, SceneFieldFlags>> sceneFieldNamesTypesFlags, const Containers::ArrayView<UnsignedInt> extraMappingOffsets, const Containers::ArrayView<UnsignedInt> extraDataOffsets, const Containers::ArrayView<UnsignedInt> extraBitOffsets, const Containers::ArrayView<UnsignedInt> extraStringOffsets, const UnsignedInt nodeI, const Containers::StringView key, const Utility::JsonToken gltfExtraValue) {
    /* If the value is an object, recurse into it. The field name will then be
       all object keys concatenated with dots. */
    if(gltfExtraValue.type() == Utility::JsonToken::Type::Object) for(const Utility::JsonObjectItem gltfNestedExtra: gltfExtraValue.asObject()) {
        parseSceneExtraFields(gltf, flags, sceneFieldsForName,
            sceneFieldNamesTypesFlags, extraMappingOffsets, extraDataOffsets,
            extraBitOffsets, extraStringOffsets, nodeI,
            "."_s.join({key, gltfNestedExtra.key()}), gltfNestedExtra.value());

    /* Scalars */
    } else if(gltfExtraValue.type() == Utility::JsonToken::Type::Bool ||
              gltfExtraValue.type() == Utility::JsonToken::Type::Number ||
              gltfExtraValue.type() == Utility::JsonToken::Type::String) {
        const UnsignedInt customFieldId = sceneFieldCustom(sceneFieldsForName.at(key));
        if(sceneFieldNamesTypesFlags[customFieldId].third() & SceneFieldFlag::MultiEntry) {
            if(!(flags & ImporterFlag::Quiet))
                Warning{} << "Trade::GltfImporter::scene(): node" << nodeI << "extras" << key << "property was expected to be an array, skipping";
            return;
        }

        bool success = false;

        {
            /* Redirect error messages from Json::parse*() to the warning
               output as they are non-fatal and only lead to given attribute
               being skipped. If quiet output is requested, don't print them at
               all. */
            Error redirectError{flags & ImporterFlag::Quiet ? nullptr : Warning::output()};

            /* Leave extraOffsets[0] and [1] at 0 to turn this into
               an offset array later */
            switch(sceneFieldNamesTypesFlags[customFieldId].second()) {
                case SceneFieldType::Bit:
                    if(gltf.parseBool(gltfExtraValue)) {
                        success = true;
                        ++extraMappingOffsets[customFieldId + 2];
                        ++extraBitOffsets[customFieldId + 2];
                    } break;
                #define _c(type) case SceneFieldType::type: \
                    if(gltf.parse ## type(gltfExtraValue)) { \
                        success = true;                         \
                        ++extraMappingOffsets[customFieldId + 2]; \
                        ++extraDataOffsets[customFieldId + 2];  \
                    } break;
                _c(Float)
                _c(UnsignedInt)
                _c(Int)
                #undef _c
                case SceneFieldType::StringOffset32:
                    if(const Containers::Optional<Containers::StringView> parsed = gltf.parseString(gltfExtraValue)) {
                        success = true;
                        ++extraMappingOffsets[customFieldId + 2];
                        ++extraDataOffsets[customFieldId + 2];
                        extraStringOffsets[customFieldId + 2] += parsed->size();
                    } break;
                default: CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
            }
        }

        if(!success && !(flags & ImporterFlag::Quiet))
            Warning{} << "Trade::GltfImporter::scene(): invalid node" << nodeI << "extras" << key << "property, skipping";

    /* Arrays, imported as multiple scalar fields */
    } else if(gltfExtraValue.type() == Utility::JsonToken::Type::Array) {
        /* Skip empty arrays -- those don't add anything to the output anyway
           so printing a warning message for them is counterproductive */
        if(gltfExtraValue.childCount() == 0)
            return;
        const Containers::Optional<Utility::JsonToken::Type> arrayType = gltfExtraValue.commonArrayType();
        if(!arrayType) {
            if(!(flags & ImporterFlag::Quiet))
                Warning{} << "Trade::GltfImporter::scene(): node" << nodeI << "extras" << key << "property is a heterogeneous array, skipping";
            return;
        }
        if(*arrayType != Utility::JsonToken::Type::Bool &&
           *arrayType != Utility::JsonToken::Type::Number &&
           *arrayType != Utility::JsonToken::Type::String) {
            if(!(flags & ImporterFlag::Quiet))
                Warning{} << "Trade::GltfImporter::scene(): node" << nodeI << "extras property is an array of" << *arrayType << Debug::nospace << ", skipping";
            return;
        }

        const UnsignedInt customFieldId = sceneFieldCustom(sceneFieldsForName.at(key));
        if(!(sceneFieldNamesTypesFlags[customFieldId].third() & SceneFieldFlag::MultiEntry)) {
            if(!(flags & ImporterFlag::Quiet))
                Warning{} << "Trade::GltfImporter::scene(): node" << nodeI << "extras" << key << "property was not expected to be an array, skipping";
            return;
        }

        bool success = false;

        {
            /* Redirect error messages from Json::parse*() to the warning
               output as they are non-fatal and only lead to given attribute
               being skipped. If quiet output is requested, don't print them at
               all. */
            Error redirectError{flags & ImporterFlag::Quiet ? nullptr : Warning::output()};

            /* Leave extraOffsets[0] and [1] at 0 to turn this into an offset
               array later */
            switch(sceneFieldNamesTypesFlags[customFieldId].second()) {
                case SceneFieldType::Bit:
                    if(const Containers::Optional<Containers::StridedBitArrayView1D> parsed = gltf.parseBitArray(gltfExtraValue)) {
                        success = true;
                        extraMappingOffsets[customFieldId + 2] += parsed->size();
                        extraBitOffsets[customFieldId + 2] += parsed->size();
                    } break;
                #define _c(type) case SceneFieldType::type: \
                    if(const Containers::Optional<Containers::StridedArrayView1D<const type>> parsed = gltf.parse ## type ## Array(gltfExtraValue)) { \
                        success = true;                             \
                        extraMappingOffsets[customFieldId + 2] += parsed->size(); \
                        extraDataOffsets[customFieldId + 2] += parsed->size(); \
                    } break;
                _c(Float)
                _c(UnsignedInt)
                _c(Int)
                #undef _c
                case SceneFieldType::StringOffset32:
                    if(const Containers::Optional<Containers::StringIterable> parsed = gltf.parseStringArray(gltfExtraValue)) {
                        success = true;
                        extraMappingOffsets[customFieldId + 2] += parsed->size();
                        extraDataOffsets[customFieldId + 2] += parsed->size();
                        for(Containers::StringView i: *parsed)
                            extraStringOffsets[customFieldId + 2] += i.size();
                    } break;
                default: CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
            }
        }

        if(!success && !(flags & ImporterFlag::Quiet))
            Warning{} << "Trade::GltfImporter::scene(): invalid node" << nodeI << "extras" << key << "array property, skipping";
    } else if(!(flags & ImporterFlag::Quiet))
        Warning{} << "Trade::GltfImporter::scene(): node" << nodeI << "extras" << key << "property is" << gltfExtraValue.type() << Debug::nospace << ", skipping";
}

void collectSceneExtraFields(const std::unordered_map<Containers::String, SceneField>& sceneFieldsForName, const Containers::ArrayView<const Containers::Triple<Containers::StringView, SceneFieldType, SceneFieldFlags>> sceneFieldNamesTypesFlags, const Containers::ArrayView<UnsignedInt> extraMappingOffsets, const Containers::ArrayView<UnsignedInt> extraMappings, const Containers::ArrayView<UnsignedInt> extraDataOffsets, const Containers::ArrayView<UnsignedInt> extrasUnsignedInt, const Containers::ArrayView<Int> extrasInt, const Containers::ArrayView<Float> extrasFloat, const Containers::ArrayView<UnsignedInt> extraBitOffsets, const Containers::MutableBitArrayView extrasBits, const Containers::ArrayView<UnsignedInt> extraStringOffsets, const Containers::ArrayView<UnsignedInt> baseStringOffsets, const Containers::MutableStringView extrasStrings, const UnsignedInt nodeI, const Containers::StringView key, const Utility::JsonToken gltfExtraValue) {
    /* If the value is an object, recurse into it. The field name will then be
       all object keys concatenated with dots. */
    if(gltfExtraValue.type() == Utility::JsonToken::Type::Object) for(const Utility::JsonObjectItem gltfNestedExtra: gltfExtraValue.asObject()) {
        collectSceneExtraFields(sceneFieldsForName, sceneFieldNamesTypesFlags,
            extraMappingOffsets, extraMappings, extraDataOffsets,
            extrasUnsignedInt, extrasInt, extrasFloat, extraBitOffsets,
            extrasBits, extraStringOffsets, baseStringOffsets, extrasStrings,
            nodeI, "."_s.join({key, gltfNestedExtra.key()}), gltfNestedExtra.value());

    /* Arrays */
    } else if(gltfExtraValue.type() == Utility::JsonToken::Type::Array) {
        const Containers::Optional<Utility::JsonToken::Type> arrayType = gltfExtraValue.commonArrayType();
        /* Skip what caused a parse error above. Since we didn't bother parsing
           arrays of types other than bool, number or string, this will skip
           them as well. */
        if(!arrayType || !gltfExtraValue.commonParsedArrayType())
            return;
        CORRADE_INTERNAL_ASSERT(
            *arrayType == Utility::JsonToken::Type::Bool ||
            *arrayType == Utility::JsonToken::Type::Number ||
            *arrayType == Utility::JsonToken::Type::String);

        const UnsignedInt customFieldId = sceneFieldCustom(sceneFieldsForName.at(key));

        /* Now the offsets are shifted by 1, after this loop they'll be shifted
           by 0 for the final SceneFieldData population. All these are done
           with a plain for loop instead of Utility::copy() because we also
           need to fill in the node index. */
        switch(sceneFieldNamesTypesFlags[customFieldId].second()) {
            case SceneFieldType::Bit: {
                UnsignedInt& extraMappingOffset = extraMappingOffsets[customFieldId + 1];
                UnsignedInt& extraBitOffset = extraBitOffsets[customFieldId + 1];
                const Containers::StridedBitArrayView1D array = gltfExtraValue.asBitArray();
                for(std::size_t i = 0; i != array.size(); ++i) {
                    extraMappings[extraMappingOffset++] = nodeI;
                    extrasBits.set(extraBitOffset++, array[i]);
                }
            } break;
            #define _c(type) case SceneFieldType::type: {       \
                UnsignedInt& extraMappingOffset = extraMappingOffsets[customFieldId + 1]; \
                UnsignedInt& extraDataOffset = extraDataOffsets[customFieldId + 1]; \
                for(const type value: gltfExtraValue.as ## type ## Array()) { \
                    extraMappings[extraMappingOffset++] = nodeI; \
                    extras ## type[extraDataOffset++] = value;  \
                }                                               \
            } break;
            _c(Float)
            _c(UnsignedInt)
            _c(Int)
            #undef _c
            case SceneFieldType::StringOffset32: {
                UnsignedInt& extraMappingOffset = extraMappingOffsets[customFieldId + 1];
                UnsignedInt& extraDataOffset = extraDataOffsets[customFieldId + 1];
                UnsignedInt& extraStringOffset = extraStringOffsets[customFieldId + 1];
                for(const Containers::StringView string: gltfExtraValue.asStringArray()) {
                    extraMappings[extraMappingOffset++] = nodeI;
                    Utility::copy(string, extrasStrings.sliceSize(extraStringOffset, string.size()));
                    extraStringOffset += string.size();
                    extrasUnsignedInt[extraDataOffset++] = extraStringOffset - baseStringOffsets[customFieldId];
                }
            } break;
            default: CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
        }

    /* Scalars */
    } else if(gltfExtraValue.isParsed() && (
        gltfExtraValue.type() == Utility::JsonToken::Type::Bool ||
        gltfExtraValue.type() == Utility::JsonToken::Type::Number ||
        gltfExtraValue.type() == Utility::JsonToken::Type::String
    )) {
        const UnsignedInt customFieldId = sceneFieldCustom(sceneFieldsForName.at(key));

        /* Now the offsets are shifted by 1, after this loop they'll be shifted
           by 0 for the final SceneFieldData population */
        switch(sceneFieldNamesTypesFlags[customFieldId].second()) {
            case SceneFieldType::Bit: {
                UnsignedInt& extraMappingOffset = extraMappingOffsets[customFieldId + 1];
                UnsignedInt& extraBitOffset = extraBitOffsets[customFieldId + 1];
                extraMappings[extraMappingOffset++] = nodeI;
                extrasBits.set(extraBitOffset++, gltfExtraValue.asBool());
            } break;
            #define _c(type) case SceneFieldType::type: {       \
                UnsignedInt& extraMappingOffset = extraMappingOffsets[customFieldId + 1]; \
                UnsignedInt& extraDataOffset = extraDataOffsets[customFieldId + 1]; \
                extraMappings[extraMappingOffset++] = nodeI;    \
                extras ## type[extraDataOffset++] = gltfExtraValue.as ## type(); \
            } break;
            _c(Float)
            _c(UnsignedInt)
            _c(Int)
            #undef _c
            case SceneFieldType::StringOffset32: {
                UnsignedInt& extraMappingOffset = extraMappingOffsets[customFieldId + 1];
                UnsignedInt& extraDataOffset = extraDataOffsets[customFieldId + 1];
                UnsignedInt& extraStringOffset = extraStringOffsets[customFieldId + 1];
                extraMappings[extraMappingOffset++] = nodeI;
                const Containers::StringView string = gltfExtraValue.asString();
                Utility::copy(string, extrasStrings.sliceSize(extraStringOffset, string.size()));
                extraStringOffset += string.size();
                extrasUnsignedInt[extraDataOffset++] = extraStringOffset - baseStringOffsets[customFieldId];
            } break;
            default: CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
        }
    }
}

}

Containers::Optional<SceneData> GltfImporter::doScene(UnsignedInt id) {
    const Utility::JsonToken gltfScene{*_d->gltf, _d->gltfScenes[id].first()};

    /* Gather all top-level nodes belonging to a scene and recursively populate
       the children ranges. Optimistically assume the glTF has just a single
       scene and reserve for that. */
    /** @todo once we have BitArrays use the objects array to mark nodes that
        are present in the scene and then create a new array from those but
        ordered so we can have OrderedMapping for parents and also all other
        fields */
    Containers::Array<UnsignedInt> objects;
    arrayReserve(objects, _d->gltfNodes.size());
    if(const Utility::JsonIterator gltfSceneNodes = gltfScene.find("nodes"_s)) {
        /* Scene node array parsed in doOpenData() already, for cycle
           detection. Bounds checked there as well, so we can just directly
           copy the contents. */
        const Containers::StridedArrayView1D<const UnsignedInt> sceneNodes = gltfSceneNodes->asUnsignedIntArray();
        Utility::copy(sceneNodes, arrayAppend(objects, NoInit, sceneNodes.size()));
    }

    /* Offset array, `children[i + 1]` to `children[i + 2]` defines a range in
       `objects` containing children of object `i`, `children[0]` to
       `children[1]` is the range of root objects with `children[0]` being
       always `0` */
    Containers::Array<UnsignedInt> children;
    arrayReserve(children, _d->gltfNodes.size() + 2);
    arrayAppend(children, {0u, UnsignedInt(objects.size())});
    for(std::size_t i = 0; i != children.size() - 1; ++i) {
        for(std::size_t j = children[i], jMax = children[i + 1]; j != jMax; ++j) {
            const Utility::JsonToken gltfNode{*_d->gltf, _d->gltfNodes[objects[j]].first()};
            if(const Utility::JsonIterator gltfNodeChildren = gltfNode.find("children"_s)) {
                /* Node children array parsed in doOpenData() already, for
                   cycle detection. Bounds checked there as well, so we can
                   just directly copy the contents. */
                const Containers::StridedArrayView1D<const UnsignedInt> nodeChildren = gltfNodeChildren->asUnsignedIntArray();
                Utility::copy(nodeChildren, arrayAppend(objects, NoInit, nodeChildren.size()));
            }
            arrayAppend(children, UnsignedInt(objects.size()));
        }
    }

    /** @todo once there's SceneData::mappingRange(), calculate also min here */
    const UnsignedInt maxObjectIndexPlusOne = objects.isEmpty() ? 0 : Math::max(objects) + 1;

    /* Count how many objects have matrices, how many have separate TRS
       properties and which of the set are present. Then also gather mesh,
       light, camera and skin assignment count. Materials have to use the same
       object mapping as meshes, so only check if there's any material
       assignment at all -- if not, then we won't need to store that field. */
    UnsignedInt transformationCount = 0;
    UnsignedInt trsCount = 0;
    bool hasTranslations = false;
    bool hasRotations = false;
    bool hasScalings = false;
    UnsignedInt meshCount = 0;
    bool hasMeshMaterials = false;
    UnsignedInt lightCount = 0;
    UnsignedInt cameraCount = 0;
    UnsignedInt skinCount = 0;
    /* Separate counter for every recognized extra field. Mappings are put into
       `extraMappingOffsets`, number and string fields are put into
       `extraDataOffsets`, bit fields into `extraBitOffsets` and string data
       into `extraStringOffsets` (i.e., an element is never non-zero in both `extraDataOffsets` and `extraBitOffsets`, in case of strings it's both
       `extraDataOffsets` and `extraStringOffsets` used). These are then turned
       into offsets into `extraMappings`, `extrasUnsignedInt ` and `extrasBits`
       arrays, for which there's two extra items at the front. */
    Containers::ArrayView<UnsignedInt> extraMappingOffsets;
    Containers::ArrayView<UnsignedInt> extraDataOffsets;
    Containers::ArrayView<UnsignedInt> extraBitOffsets;
    Containers::ArrayView<UnsignedInt> extraStringOffsets;
    /* Stores a copy of extraStringOffsets, see detailed comment when populated
       below */
    Containers::ArrayView<UnsignedInt> baseStringOffsets;
    Containers::ArrayTuple extraOffsetStorage{
        {ValueInit, _d->sceneFieldNamesTypesFlags.size() + 2, extraMappingOffsets},
        {ValueInit, _d->sceneFieldNamesTypesFlags.size() + 2, extraDataOffsets},
        {ValueInit, _d->sceneFieldNamesTypesFlags.size() + 2, extraBitOffsets},
        {ValueInit, _d->sceneFieldNamesTypesFlags.size() + 2, extraStringOffsets},
        {ValueInit, _d->sceneFieldNamesTypesFlags.size(), baseStringOffsets}
    };
    for(const UnsignedInt i: objects) {
        const Utility::JsonToken gltfNode{*_d->gltf, _d->gltfNodes[i].first()};

        /* Cache repeated queries to not suffer from the O(n) lookup too much */
        const bool hasTranslation = !!gltfNode.find("translation"_s);
        const bool hasRotation = !!gltfNode.find("rotation"_s);
        const bool hasScaling = !!gltfNode.find("scale"_s);

        /* Everything that has a TRS should have a transformation matrix as
           well. OTOH there can be a transformation matrix but no TRS, and
           there can also be objects without any transformation. */
        if(hasTranslation || hasRotation || hasScaling) {
            ++trsCount;
            ++transformationCount;
        } else if(gltfNode.find("matrix"_s))
            ++transformationCount;

        if(hasTranslation)
            hasTranslations = true;
        if(hasRotation)
            hasRotations = true;
        if(hasScaling)
            hasScalings = true;

        /* Mesh reference */
        if(const Utility::JsonIterator gltfMesh = gltfNode.find("mesh"_s)) {
            if(!_d->gltf->parseUnsignedInt(*gltfMesh)) {
                Error{} << "Trade::GltfImporter::scene(): invalid mesh property of node" << i;
                return {};
            }
            const UnsignedInt mesh = gltfMesh->asUnsignedInt();
            if(mesh >= _d->gltfMeshes.size()) {
                Error{} << "Trade::GltfImporter::scene(): mesh index" << mesh << "in node" << i << "out of range for" << _d->gltfMeshes.size() << "meshes";
                return {};
            }

            meshCount += _d->meshSizeOffsets[mesh + 1] - _d->meshSizeOffsets[mesh];
            for(std::size_t j = _d->meshSizeOffsets[mesh], jMax = _d->meshSizeOffsets[mesh + 1]; j != jMax; ++j) {
                if(const Utility::JsonIterator gltfPrimitiveMaterial = Utility::JsonToken{*_d->gltf, _d->gltfMeshPrimitiveMap[j].second()}.find("material"_s)) {
                    if(!_d->gltf->parseUnsignedInt(*gltfPrimitiveMaterial)) {
                        Error{} << "Trade::GltfImporter::scene(): invalid material property of mesh" << mesh << "primitive" << j - _d->meshSizeOffsets[mesh];
                        return {};
                    }
                    if(gltfPrimitiveMaterial->asUnsignedInt() >= _d->gltfMaterials.size()) {
                        Error{} << "Trade::GltfImporter::scene(): material index" << gltfPrimitiveMaterial->asUnsignedInt() << "in mesh" << mesh << "primitive" << j - _d->meshSizeOffsets[mesh] << "out of range for" << _d->gltfMaterials.size() << "materials";
                        return {};
                    }

                    hasMeshMaterials = true;
                    /* No break here to ensure parsing and checks are is called
                       on materials of all primitives */
                }
            }
        }

        /* Camera reference */
        if(const Utility::JsonIterator gltfCamera = gltfNode.find("camera"_s)) {
            if(!_d->gltf->parseUnsignedInt(*gltfCamera)) {
                Error{} << "Trade::GltfImporter::scene(): invalid camera property of node" << i;
                return {};
            }
            if(gltfCamera->asUnsignedInt() >= _d->gltfCameras.size()) {
                Error{} << "Trade::GltfImporter::scene(): camera index" << gltfCamera->asUnsignedInt() << "in node" << i << "out of range for" << _d->gltfCameras.size() << "cameras";
                return {};
            }

            ++cameraCount;
        }

        /* Skin reference */
        if(const Utility::JsonIterator gltfSkin = gltfNode.find("skin"_s)) {
            if(!_d->gltf->parseUnsignedInt(*gltfSkin)) {
                Error{} << "Trade::GltfImporter::scene(): invalid skin property of node" << i;
                return {};
            }
            if(gltfSkin->asUnsignedInt() >= _d->gltfSkins.size()) {
                Error{} << "Trade::GltfImporter::scene(): skin index" << gltfSkin->asUnsignedInt() << "in node" << i << "out of range for" << _d->gltfSkins.size() << "skins";
                return {};
            }

            ++skinCount;
        }

        /* Extensions */
        if(const Utility::JsonIterator gltfExtensions = gltfNode.find("extensions"_s)) {
            if(!_d->gltf->parseObject(*gltfExtensions)) {
                Error{} << "Trade::GltfImporter::scene(): invalid node" << i << "extensions property";
                return {};
            }

            /* Light reference */
            if(const Utility::JsonIterator gltfKhrLightsPunctual = gltfExtensions->find("KHR_lights_punctual"_s)) {
                if(!_d->gltf->parseObject(*gltfKhrLightsPunctual)) {
                    Error{} << "Trade::GltfImporter::scene(): invalid node" << i << "KHR_lights_punctual extension";
                    return {};
                }

                const Utility::JsonIterator gltfLight = gltfKhrLightsPunctual->find("light"_s);
                if(!gltfLight || !_d->gltf->parseUnsignedInt(*gltfLight)) {
                    Error{} << "Trade::GltfImporter::scene(): missing or invalid KHR_lights_punctual light property of node" << i;
                    return {};
                }
                if(gltfLight->asUnsignedInt() >= _d->gltfLights.size()) {
                    Error{} << "Trade::GltfImporter::scene(): light index" << gltfLight->asUnsignedInt() << "in node" << i << "out of range for" << _d->gltfLights.size() << "lights";
                    return {};
                }

                ++lightCount;
            }
        }

        /* Extras. If it's an object, it was already parsed during initial
           import */
        if(const Utility::JsonIterator gltfExtras = gltfNode.find("extras"_s)) {
            /* The process is recursive so it has to be an external function */
            if(gltfExtras->type() == Utility::JsonToken::Type::Object) for(const Utility::JsonObjectItem gltfExtra: gltfExtras->asObject()) {
                parseSceneExtraFields(*_d->gltf, flags(), _d->sceneFieldsForName, _d->sceneFieldNamesTypesFlags, extraMappingOffsets, extraDataOffsets, extraBitOffsets, extraStringOffsets, i, gltfExtra.key(), gltfExtra.value());
            } else if(!(flags() & ImporterFlag::Quiet))
                Warning{} << "Trade::GltfImporter::scene(): node" << i << "extras property is" << gltfExtras->type() << Debug::nospace << ", skipping";
        }
    }

    /* Turn the `extra*Offsets` into an offset array. After this step,
       `extra*Offsets[i + 1]` to `extra*Offsets[i + 2]` is the range of data
       for extra field `sceneFieldCustom(i)`; `extra*Offsets[0]` and `[1]` is
       0. */
    std::size_t extraMappingCount = 0;
    std::size_t extraDataCount = 0;
    std::size_t extraBitCount = 0;
    std::size_t extraStringSize = 0;
    for(UnsignedInt& i: extraMappingOffsets) {
        const UnsignedInt count = i;
        i += extraMappingCount;
        extraMappingCount += count;
    }
    for(UnsignedInt& i: extraDataOffsets) {
        const UnsignedInt count = i;
        i += extraDataCount;
        extraDataCount += count;
    }
    for(UnsignedInt& i: extraBitOffsets) {
        const UnsignedInt count = i;
        i += extraBitCount;
        extraBitCount += count;
    }
    for(UnsignedInt& i: extraStringOffsets) {
        const UnsignedInt count = i;
        i += extraStringSize;
        extraStringSize += count;
    }

    /* Remember base offsets for string fields by copying them to a separate
       array, as extraStringOffsets will get further modified below. While all
       strings are in a single contiguous array, fields index into them with
       offsets that's relative to the base offset for given field. */
    for(std::size_t i = 0; i != _d->sceneFieldNamesTypesFlags.size(); ++i)
        baseStringOffsets[i] = extraStringOffsets[i + 1];

    /** @todo switch to 64-bit offsets if there's many strings */
    CORRADE_INTERNAL_ASSERT(extraStringSize <= ~UnsignedInt{});

    /* If all objects that have transformations have TRS as well, no need to
       store the combined transform field */
    if(trsCount == transformationCount)
        transformationCount = 0;

    /* Allocate the output array */
    Containers::ArrayView<UnsignedInt> parentImporterStateObjects;
    Containers::ArrayView<Int> parents;
    Containers::ArrayView<const Utility::JsonTokenData*> importerState;
    Containers::ArrayView<UnsignedInt> transformationObjects;
    Containers::ArrayView<Matrix4> transformations;
    Containers::ArrayView<UnsignedInt> trsObjects;
    Containers::ArrayView<Vector3> translations;
    Containers::ArrayView<Quaternion> rotations;
    Containers::ArrayView<Vector3> scalings;
    Containers::ArrayView<UnsignedInt> meshMaterialObjects;
    Containers::ArrayView<UnsignedInt> meshes;
    Containers::ArrayView<Int> meshMaterials;
    Containers::ArrayView<UnsignedInt> lightObjects;
    Containers::ArrayView<UnsignedInt> lights;
    Containers::ArrayView<UnsignedInt> cameraObjects;
    Containers::ArrayView<UnsignedInt> cameras;
    Containers::ArrayView<UnsignedInt> skinObjects;
    Containers::ArrayView<UnsignedInt> skins;
    Containers::ArrayView<UnsignedInt> extraMappings;
    Containers::MutableStringView extrasStrings;
    /* This gets later cast to extrasFloat and extrasInt */
    /** @todo Abusing the fact that all allowed extras types are 32-bit now,
        when 64-bit types are introduced there has to be a second 64-bit array
        to satisfy alignment. For composite types (pairs, vectors, matrices)
        however it's enough to just take more items at once. */
    Containers::ArrayView<UnsignedInt> extrasUnsignedInt;
    Containers::MutableBitArrayView extrasBits;
    Containers::Array<char> data = Containers::ArrayTuple{
        {NoInit, objects.size(), parentImporterStateObjects},
        {NoInit, objects.size(), parents},
        {NoInit, objects.size(), importerState},
        {NoInit, transformationCount, transformationObjects},
        {NoInit, transformationCount, transformations},
        {NoInit, trsCount, trsObjects},
        {NoInit, hasTranslations ? trsCount : 0, translations},
        {NoInit, hasRotations ? trsCount : 0, rotations},
        {NoInit, hasScalings ? trsCount : 0, scalings},
        {NoInit, meshCount, meshMaterialObjects},
        {NoInit, meshCount, meshes},
        {NoInit, hasMeshMaterials ? meshCount : 0, meshMaterials},
        {NoInit, lightCount, lightObjects},
        {NoInit, lightCount, lights},
        {NoInit, cameraCount, cameraObjects},
        {NoInit, cameraCount, cameras},
        {NoInit, skinCount, skinObjects},
        {NoInit, skinCount, skins},
        {NoInit, extraMappingCount, extraMappings},
        {NoInit, extraStringSize, extrasStrings},
        {NoInit, extraDataCount, extrasUnsignedInt},
        {NoInit, extraBitCount, extrasBits},
    };
    const auto extrasFloat = Containers::arrayCast<Float>(extrasUnsignedInt);
    const auto extrasInt = Containers::arrayCast<Int>(extrasUnsignedInt);

    /* Populate object mapping for parents and importer state, synthesize
       parent info from the child ranges */
    Utility::copy(objects, parentImporterStateObjects);
    for(std::size_t i = 0; i != children.size() - 1; ++i) {
        Int parent = Int(i) - 1;
        for(std::size_t j = children[i], jMax = children[i + 1]; j != jMax; ++j)
            parents[j] = parent == -1 ? -1 : objects[parent];
    }

    /* Populate the rest */
    std::size_t transformationOffset = 0;
    std::size_t trsOffset = 0;
    std::size_t meshMaterialOffset = 0;
    std::size_t lightOffset = 0;
    std::size_t cameraOffset = 0;
    std::size_t skinOffset = 0;
    for(std::size_t i = 0; i != objects.size(); ++i) {
        const UnsignedInt nodeI = objects[i];
        const Utility::JsonToken gltfNode{*_d->gltf, _d->gltfNodes[nodeI].first()};

        /* Populate importer state */
        importerState[i] = &gltfNode.token();

        /* Parse TRS */
        Vector3 translation;
        const Utility::JsonIterator gltfTranslation = gltfNode.find("translation"_s);
        if(gltfTranslation) {
            const Containers::Optional<Containers::StridedArrayView1D<const float>> translationArray = _d->gltf->parseFloatArray(*gltfTranslation, 3);
            if(!translationArray) {
                Error{} << "Trade::GltfImporter::scene(): invalid translation property of node" << nodeI;
                return {};
            }

            Utility::copy(*translationArray, translation.data());
        }

        Quaternion rotation;
        const Utility::JsonIterator gltfRotation = gltfNode.find("rotation"_s);
        if(gltfRotation) {
            const Containers::Optional<Containers::StridedArrayView1D<const float>> rotationArray = _d->gltf->parseFloatArray(*gltfRotation, 4);
            if(!rotationArray) {
                Error{} << "Trade::GltfImporter::scene(): invalid rotation property of node" << nodeI;
                return {};
            }

            /* glTF also uses the XYZW order */
            Utility::copy(*rotationArray, rotation.data());
            if(!rotation.isNormalized() && configuration().value<bool>("normalizeQuaternions")) {
                rotation = rotation.normalized();
                if(!(flags() & ImporterFlag::Quiet))
                    Warning{} << "Trade::GltfImporter::scene(): rotation quaternion of node" << nodeI << "was renormalized";
            }
        }

        Vector3 scaling{1.0f};
        const Utility::JsonIterator gltfScale = gltfNode.find("scale"_s);
        if(gltfScale) {
            const Containers::Optional<Containers::StridedArrayView1D<const float>> scalingArray = _d->gltf->parseFloatArray(*gltfScale, 3);
            if(!scalingArray) {
                Error{} << "Trade::GltfImporter::scene(): invalid scale property of node" << nodeI;
                return {};
            }

            Utility::copy(*scalingArray, scaling.data());
        }

        /* Parse transformation, or combine it from TRS if not present */
        Matrix4 transformation;
        const Utility::JsonIterator gltfMatrix = gltfNode.find("matrix"_s);
        if(gltfMatrix) {
            const Containers::Optional<Containers::StridedArrayView1D<const float>> transformationArray = _d->gltf->parseFloatArray(*gltfMatrix, 16);
            if(!transformationArray) {
                Error{} << "Trade::GltfImporter::scene(): invalid matrix property of node" << nodeI;
                return {};
            }

            Utility::copy(*transformationArray, transformation.data());
        } else transformation =
            Matrix4::translation(translation)*
            Matrix4{rotation.toMatrix()}*
            Matrix4::scaling(scaling);

        /* Populate the combined transformation and object mapping only if
           there's actually some transformation for this object and we want to
           store it -- if all objects have TRS anyway, the matrix is redundant */
        if((gltfMatrix ||
            gltfTranslation ||
            gltfRotation ||
            gltfScale) && transformationCount)
        {
            transformations[transformationOffset] = transformation;
            transformationObjects[transformationOffset] = nodeI;
            ++transformationOffset;
        }

        /* Store the TRS information and object mapping only if there was
           something */
        if(gltfTranslation || gltfRotation || gltfScale) {
            if(hasTranslations)
                translations[trsOffset] = translation;
            if(hasRotations)
                rotations[trsOffset] = rotation;
            if(hasScalings)
                scalings[trsOffset] = scaling;
            trsObjects[trsOffset] = nodeI;
            ++trsOffset;
        }

        /* Populate mesh references. All parsing and bounds checks done in the
           previous pass already. */
        if(const Utility::JsonIterator gltfMesh = gltfNode.find("mesh"_s)) {
            const UnsignedInt mesh = gltfMesh->asUnsignedInt();
            for(std::size_t j = _d->meshSizeOffsets[mesh], jMax = _d->meshSizeOffsets[mesh + 1]; j != jMax; ++j) {
                meshMaterialObjects[meshMaterialOffset] = nodeI;
                meshes[meshMaterialOffset] = j;
                if(const Utility::JsonIterator gltfPrimitiveMaterial = Utility::JsonToken{*_d->gltf, _d->gltfMeshPrimitiveMap[j].second()}.find("material"_s)) {
                    meshMaterials[meshMaterialOffset] = gltfPrimitiveMaterial->asUnsignedInt();
                } else if(hasMeshMaterials)
                    meshMaterials[meshMaterialOffset] = -1;
                ++meshMaterialOffset;
            }
        }

        /* Populate camera references. Parsing and bounds check done in the
           previous pass already. */
        if(const Utility::JsonIterator gltfCamera = gltfNode.find("camera"_s)) {
            cameraObjects[cameraOffset] = nodeI;
            cameras[cameraOffset] = gltfCamera->asUnsignedInt();
            ++cameraOffset;
        }

        /* Populate skin references. Parsing and bounds check done in the
           previous pass already. */
        if(const Utility::JsonIterator gltfSkin = gltfNode.find("skin"_s)) {
            skinObjects[skinOffset] = nodeI;
            skins[skinOffset] = gltfSkin->asUnsignedInt();
            ++skinOffset;
        }

        /* Extensions. Type of the property checked in the previous pass
           already. */
        if(const Utility::JsonIterator gltfExtensions = gltfNode.find("extensions"_s)) {
            /* Populate light references. Property type, parsing and bounds
               check done in the previous pass already. */
            if(const Utility::JsonIterator gltfKhrLightsPunctual = gltfExtensions->find("KHR_lights_punctual"_s)) {
                lightObjects[lightOffset] = nodeI;
                lights[lightOffset] = (*gltfKhrLightsPunctual)["light"_s].asUnsignedInt();
                ++lightOffset;
            }
        }

        /* Extras. Types were checked in the previous pass already, so just
           skip if it's not an object or if the number is not parsed (i.e.,
           an invalid literal). */
        if(const Utility::JsonIterator gltfExtras = gltfNode.find("extras"_s)) {
            /* The process is recursive so it has to be an external function */
            if(gltfExtras->type() == Utility::JsonToken::Type::Object) for(const Utility::JsonObjectItem gltfExtra: gltfExtras->asObject()) {
                collectSceneExtraFields(_d->sceneFieldsForName, _d->sceneFieldNamesTypesFlags, extraMappingOffsets, extraMappings, extraDataOffsets, extrasUnsignedInt, extrasInt, extrasFloat, extraBitOffsets, extrasBits, extraStringOffsets, baseStringOffsets, extrasStrings, nodeI, gltfExtra.key(), gltfExtra.value());
            }
        }
    }

    CORRADE_INTERNAL_ASSERT(
        transformationOffset == transformations.size() &&
        trsOffset == trsObjects.size() &&
        meshMaterialOffset == meshMaterialObjects.size() &&
        lightOffset == lightObjects.size() &&
        cameraOffset == cameraObjects.size() &&
        skinOffset == skinObjects.size());

    /* Put everything together. For simplicity the imported data could always
       have all fields present, with some being empty, but this gives less
       noise for asset introspection purposes. */
    Containers::Array<SceneFieldData> fields;
    arrayAppend(fields, {
        /** @todo once there's a flag to annotate implicit fields, omit the
            parent field if it's all -1s; or alternatively we could also have a
            stride of 0 for this case */
        SceneFieldData{SceneField::Parent, parentImporterStateObjects, parents},
        SceneFieldData{SceneField::ImporterState, parentImporterStateObjects, importerState}
    });

    /* Transformations. If there's no such field, add an empty transformation
       to indicate it's a 3D scene. */
    if(transformationCount) arrayAppend(fields, SceneFieldData{
        SceneField::Transformation, transformationObjects, transformations
    });
    if(hasTranslations) arrayAppend(fields, SceneFieldData{
        SceneField::Translation, trsObjects, translations
    });
    if(hasRotations) arrayAppend(fields, SceneFieldData{
        SceneField::Rotation, trsObjects, rotations
    });
    if(hasScalings) arrayAppend(fields, SceneFieldData{
        SceneField::Scaling, trsObjects, scalings
    });
    if(!transformationCount && !trsCount) arrayAppend(fields, SceneFieldData{
        SceneField::Transformation, SceneMappingType::UnsignedInt, nullptr, SceneFieldType::Matrix4x4, nullptr
    });

    /* Multiple meshes (and materials) can be attached to a glTF node, but not
       anything else */
    if(meshCount) arrayAppend(fields, SceneFieldData{
        SceneField::Mesh, meshMaterialObjects, meshes, SceneFieldFlag::MultiEntry,
    });
    if(hasMeshMaterials) arrayAppend(fields, SceneFieldData{
        SceneField::MeshMaterial, meshMaterialObjects, meshMaterials, SceneFieldFlag::MultiEntry,
    });
    if(lightCount) arrayAppend(fields, SceneFieldData{
        SceneField::Light, lightObjects, lights
    });
    if(cameraCount) arrayAppend(fields, SceneFieldData{
        SceneField::Camera, cameraObjects, cameras
    });
    if(skinCount) arrayAppend(fields, SceneFieldData{
        SceneField::Skin, skinObjects, skins
    });

    /* Extras. At this point, `extraOffsets[i]` to `extraOffsets[i + 1]` is the
       range of data for extra field sceneFieldCustom(i). Add it if it's
       non-empty. */
    for(std::size_t i = 0; i != _d->sceneFieldNamesTypesFlags.size(); ++i) {
        const SceneFieldType fieldType = _d->sceneFieldNamesTypesFlags[i].second();
        const SceneFieldFlags fieldFlags = _d->sceneFieldNamesTypesFlags[i].third();
        const Containers::StridedArrayView1D<UnsignedInt> mapping = extraMappings.slice(extraMappingOffsets[i], extraMappingOffsets[i + 1]);
        std::size_t dataBegin, dataEnd;
        if(fieldType == SceneFieldType::Bit) {
            dataBegin = extraBitOffsets[i];
            dataEnd = extraBitOffsets[i + 1];
        } else {
            dataBegin = extraDataOffsets[i];
            dataEnd = extraDataOffsets[i + 1];
        }
        if(dataBegin != dataEnd) {
            if(fieldType == SceneFieldType::StringOffset32)
                arrayAppend(fields, SceneFieldData{sceneFieldCustom(i),
                SceneMappingType::UnsignedInt, mapping,
                /* At this point, extraStringOffsets is the same as
                   baseStringOffsets, it doesn't matter which one is used */
                extrasStrings.data() + baseStringOffsets[i], fieldType, extrasUnsignedInt.slice(dataBegin, dataEnd), fieldFlags});
            else if(fieldType == SceneFieldType::Bit)
                arrayAppend(fields, SceneFieldData{sceneFieldCustom(i),
                SceneMappingType::UnsignedInt, mapping,
                extrasBits.slice(dataBegin, dataEnd), fieldFlags});
            else arrayAppend(fields, SceneFieldData{sceneFieldCustom(i),
                SceneMappingType::UnsignedInt, mapping,
                fieldType, extrasUnsignedInt.slice(dataBegin, dataEnd), fieldFlags});
        }
    }

    /* Convert back to the default deleter to avoid dangling deleter function
       pointer issues when unloading the plugin */
    arrayShrink(fields, DefaultInit);
    /* Even though SceneData is capable of holding more than 4 billion objects,
       we realistically don't expect glTF to have that many -- the text file
       would be *terabytes* then */
    return SceneData{SceneMappingType::UnsignedInt, maxObjectIndexPlusOne, Utility::move(data), Utility::move(fields), &gltfScene.token()};
}

SceneField GltfImporter::doSceneFieldForName(const Containers::StringView name) {
    return _d ? _d->sceneFieldsForName[name] : SceneField{};
}

Containers::String GltfImporter::doSceneFieldName(const SceneField name) {
    return _d && sceneFieldCustom(name) < _d->sceneFieldNamesTypesFlags.size() ?
        _d->sceneFieldNamesTypesFlags[sceneFieldCustom(name)].first() : ""_s;
}

UnsignedLong GltfImporter::doObjectCount() const {
    return _d->gltfNodes.size();
}

Long GltfImporter::doObjectForName(const Containers::StringView name) {
    if(!_d->nodesForName) {
        _d->nodesForName.emplace();
        _d->nodesForName->reserve(_d->gltfNodes.size());
        for(std::size_t i = 0; i != _d->gltfNodes.size(); ++i) {
            if(const Containers::StringView n = _d->gltfNodes[i].second())
                _d->nodesForName->emplace(n, i);
        }
    }

    const auto found = _d->nodesForName->find(name);
    return found == _d->nodesForName->end() ? -1 : found->second;
}

Containers::String GltfImporter::doObjectName(const UnsignedLong id) {
    return _d->gltfNodes[id].second();
}

UnsignedInt GltfImporter::doSkin3DCount() const {
    return _d->gltfSkins.size();
}

Int GltfImporter::doSkin3DForName(const Containers::StringView name) {
    if(!_d->skinsForName) {
        _d->skinsForName.emplace();
        _d->skinsForName->reserve(_d->gltfSkins.size());
        for(std::size_t i = 0; i != _d->gltfSkins.size(); ++i)
            if(const Containers::StringView n = _d->gltfSkins[i].second())
                _d->skinsForName->emplace(n, i);
    }

    const auto found = _d->skinsForName->find(name);
    return found == _d->skinsForName->end() ? -1 : found->second;
}

Containers::String GltfImporter::doSkin3DName(const UnsignedInt id) {
    return _d->gltfSkins[id].second();
}

Containers::Optional<SkinData3D> GltfImporter::doSkin3D(const UnsignedInt id) {
    const Utility::JsonToken gltfSkin{*_d->gltf, _d->gltfSkins[id].first()};

    /* Joint IDs */
    const Utility::JsonIterator gltfJoints = gltfSkin.find("joints"_s);
    Containers::Optional<Containers::StridedArrayView1D<const UnsignedInt>> jointsArray;
    if(!gltfJoints || !(jointsArray = _d->gltf->parseUnsignedIntArray(*gltfJoints))) {
        Error{} << "Trade::GltfImporter::skin3D(): missing or invalid joints property";
        return {};
    }
    if(jointsArray->isEmpty()) {
        Error{} << "Trade::GltfImporter::skin3D(): skin has no joints";
        return {};
    }
    Containers::Array<UnsignedInt> joints{NoInit, jointsArray->size()};
    for(std::size_t i = 0; i != jointsArray->size(); ++i) {
        const UnsignedInt joint = (*jointsArray)[i];
        if(joint >= _d->gltfNodes.size()) {
            Error{} << "Trade::GltfImporter::skin3D(): joint index" << joint << "out of range for" << _d->gltfNodes.size() << "nodes";
            return {};
        }

        joints[i] = joint;
    }

    /* Inverse bind matrices. If there are none, default is identities */
    Containers::Array<Matrix4> inverseBindMatrices{ValueInit, joints.size()};
    if(const Utility::JsonIterator gltfInverseBindMatrices = gltfSkin.find("inverseBindMatrices"_s)) {
        if(!_d->gltf->parseUnsignedInt(*gltfInverseBindMatrices)) {
            Error{} << "Trade::GltfImporter::skin3D(): invalid inverseBindMatrices property";
            return {};
        }

        const Containers::Optional<Accessor> accessor = parseAccessor("Trade::GltfImporter::skin3D():", gltfInverseBindMatrices->asUnsignedInt());
        if(!accessor)
            return {};

        /* There's no technical reason this couldn't work, it's just that I
           don't see any practical use case that would warrant the extra
           testing effort, so just fail for now */
        if(accessor->bufferView == ~UnsignedInt{}) {
            Error{} << "Trade::GltfImporter::skin3D(): accessor" << gltfInverseBindMatrices->asUnsignedInt() << "has no buffer view, which is unsupported";
            return {};
        }
        if(accessor->sparseValues.data()) {
            Error{} << "Trade::GltfImporter::skin3D(): accessor" << gltfInverseBindMatrices->asUnsignedInt() << "is using sparse storage, which is unsupported";
            return {};
        }

        if(accessor->format != VertexFormat::Matrix4x4) {
            /* Since we're abusing VertexFormat for all formats, print just the
               enum value without the prefix to avoid cofusion */
            Error{} << "Trade::GltfImporter::skin3D(): inverse bind matrices have unexpected type" << Debug::packed << accessor->format;
            return {};
        }

        Containers::StridedArrayView1D<const Matrix4> matrices = Containers::arrayCast<1, const Matrix4>(accessor->data);
        if(matrices.size() != inverseBindMatrices.size()) {
            Error{} << "Trade::GltfImporter::skin3D(): invalid inverse bind matrix count, expected" << inverseBindMatrices.size() << "but got" << matrices.size();
            return {};
        }

        Utility::copy(matrices, inverseBindMatrices);
    }

    return SkinData3D{Utility::move(joints), Utility::move(inverseBindMatrices), &gltfSkin.token()};
}

UnsignedInt GltfImporter::doMeshCount() const {
    return _d->gltfMeshPrimitiveMap.size();
}

Int GltfImporter::doMeshForName(const Containers::StringView name) {
    /* As we can't fail here, name strings were parsed during import already
       (with the assumption they're mostly not escaped and thus overhead-less),
       but the map is populated lazily as that *is* some work */
    if(!_d->meshesForName) {
        _d->meshesForName.emplace();
        _d->meshesForName->reserve(_d->gltfMeshes.size());
        for(std::size_t i = 0; i != _d->gltfMeshes.size(); ++i) {
            if(const Containers::StringView n = _d->gltfMeshes[i].second())
                /* The mesh can be duplicated for as many primitives as it has,
                   point to the first mesh in the duplicate sequence */
                _d->meshesForName->emplace(n, _d->meshSizeOffsets[i]);
        }
    }

    const auto found = _d->meshesForName->find(name);
    return found == _d->meshesForName->end() ? -1 : found->second;
}

Containers::String GltfImporter::doMeshName(const UnsignedInt id) {
    /* This returns the same name for all multi-primitive mesh duplicates */
    return _d->gltfMeshes[_d->gltfMeshPrimitiveMap[id].first()].second();
}

namespace {

/* Used in doMesh() and doMaterial() to remove duplicate keys from a JSON
   object. It doesn't use std::stable_sort() to preserve the order of
   duplicates but rather requires the comparator to be written in a way that no
   two values compare equal. Given that the things we're sorting are pointing
   back to the input JSON string this is easily achievable and avoids a nasty
   extra allocation somewhere deep inside STL. Then, all duplicates except the
   last one are removed, consistently with what cgltf or json.hpp does. */
/** @todo drop "all except last" and use only the first, as that's what the
    Utility::JsonToken::find() do */
template<class T, class F, class G> std::size_t stableSortRemoveDuplicatesToPrefix(const Containers::ArrayView<T> container, F lessThanComparator, G equalComparator) {
    std::sort(container.begin(), container.end(), lessThanComparator);
    #ifndef CORRADE_NO_DEBUG_ASSERT
    for(std::size_t i = 0; i + 1 < container.size(); ++i)
        CORRADE_INTERNAL_DEBUG_ASSERT(lessThanComparator(container[i], container[i + 1]));
    #endif
    const auto reversed = stridedArrayView(container).template flipped<0>();
    return std::unique(reversed.begin(), reversed.end(), equalComparator) - reversed.begin();
}

}

Containers::Optional<MeshData> GltfImporter::doMesh(const UnsignedInt id, UnsignedInt) {
    const Utility::JsonToken gltfPrimitive{*_d->gltf, _d->gltfMeshPrimitiveMap[id].second()};

    /* Primitive is optional, defaulting to triangles */
    MeshPrimitive primitive = MeshPrimitive::Triangles;
    if(const Utility::JsonIterator gltfMode = gltfPrimitive.find("mode"_s)) {
        if(!_d->gltf->parseUnsignedInt(*gltfMode)) {
            Error{} << "Trade::GltfImporter::mesh(): invalid primitive mode property";
            return {};
        }
        switch(gltfMode->asUnsignedInt()) {
            case Implementation::GltfModePoints:
                primitive = MeshPrimitive::Points;
                break;
            case Implementation::GltfModeLines:
                primitive = MeshPrimitive::Lines;
                break;
            case Implementation::GltfModeLineLoop:
                primitive = MeshPrimitive::LineLoop;
                break;
            case Implementation::GltfModeLineStrip:
                primitive = MeshPrimitive::LineStrip;
                break;
            case Implementation::GltfModeTriangles:
                primitive = MeshPrimitive::Triangles;
                break;
            case Implementation::GltfModeTriangleStrip:
                primitive = MeshPrimitive::TriangleStrip;
                break;
            case Implementation::GltfModeTriangleFan:
                primitive = MeshPrimitive::TriangleFan;
                break;
            default:
                Error{} << "Trade::GltfImporter::mesh(): unrecognized primitive" << gltfMode->asUnsignedInt();
                return {};
        }
    }

    /* Attributes. Not storing Utility::JsonTokenData& as this is just a
       temporary container so twice the size doesn't matter and creating a
       JsonToken on-the-fly in the hot std::sort() loop doesn't make sense
       perf-wise. */
    struct Attribute {
        Containers::StringView name;
        Utility::JsonToken value;
        Int morphTargetId;
    };
    Containers::Array<Attribute> attributeOrder;
    if(const Utility::JsonIterator gltfAttributes = gltfPrimitive.find("attributes"_s)) {
        /* Primitive attributes object parsed in doOpenData() already, for
           custom attribute discovery, so we just use it directly. */
        for(Utility::JsonObjectItem gltfAttribute: gltfAttributes->asObject()) {
            if(!_d->gltf->parseUnsignedInt(gltfAttribute.value())) {
                Error{} << "Trade::GltfImporter::mesh(): invalid attribute" << gltfAttribute.key();
                return {};
            }
            /* Bounds check is done in parseAccessor() later, no need to do it
               here again */
            arrayAppend(attributeOrder, InPlaceInit, gltfAttribute.key(), gltfAttribute.value(), -1);
        }
    }

    /* 3.7.2.1 (Geometry § Meshes § Overview) says "Primitives specify one or
       more attributes", but we allow also none unless the strict option is
       enabled. Not printing a warning if the strict option is disabled as
       Magnum can handle the attribute-less MeshData just fine. */
    if(attributeOrder.isEmpty() && configuration().value<bool>("strict")) {
        Error{} << "Trade::GltfImporter::mesh(): strict mode enabled, disallowing a mesh with no attributes";
        return {};
    }

    /* Morph target attributes */
    if(const Utility::JsonIterator gltfTargets = gltfPrimitive.find("targets"_s)) {
        /* Morph targets array and target attribute objects parsed in
           doOpenData() already, for custom attribute discovery, so we just use
           it directly. */
        for(Utility::JsonArrayItem gltfTarget: gltfTargets->asArray()) {
            for(Utility::JsonObjectItem gltfMorphAttribute: gltfTarget.value().asObject()) {
                if(!_d->gltf->parseUnsignedInt(gltfMorphAttribute.value())) {
                    Error{} << "Trade::GltfImporter::mesh(): invalid morph target attribute" << gltfMorphAttribute.key();
                    return {};
                }

                /* Another option would be importing just the first 128, for
                   example, but if a file has this many it's likely not going
                   to render properly anyway. Without this check, it'd blow up
                   on an assert in MeshAttributeData later. */
                if(gltfTarget.index() >= 128) {
                    Error{} << "Trade::GltfImporter::mesh(): only 128 morph targets are supported at most";
                    return {};
                }

                arrayAppend(attributeOrder, InPlaceInit, gltfMorphAttribute.key(), gltfMorphAttribute.value(), Int(gltfTarget.index()));
            }
        }
    }

    /* Sort and remove duplicates except the last one. Attributes sorted by
       name (per morph target) so that we add attribute sets in the correct
       order and can warn if indices are not contiguous. */
    const std::size_t uniqueAttributeCount = stableSortRemoveDuplicatesToPrefix(arrayView(attributeOrder),
        [](const Attribute& a, const Attribute& b) {
            /* If the morph targets are different, sort by those */
            if(a.morphTargetId != b.morphTargetId)
                return a.morphTargetId < b.morphTargetId;
            /* Otherwise, if the names are different, sort by those */
            if(a.name != b.name)
                return a.name < b.name;
            /* Otherwise, sort the earlier occurence first. Cannot use
               {a,b}.name.data() since the key can contain an escape character,
               which would then cause the string to be allocated elsewhere
               instead of pointing to the JSON input, making the order random
               again. The token data string view data pointer however *is*
               pointing to the JSON input always. The token address itself
               could also since they're stored in a contiguous array, but this
               is more robust. */
            return a.value.data().data() < b.value.data().data();
        },
        [](const Attribute& a, const Attribute& b) {
            return a.morphTargetId == b.morphTargetId && a.name == b.name;
        });
    /** @todo use suffix() once it takes suffix size and not prefix size */
    const Containers::ArrayView<const Attribute> uniqueAttributeOrder = attributeOrder.exceptPrefix(attributeOrder.size() - uniqueAttributeCount);

    /* Buffer ranges spanning all attributes. After collecting them for all
       attributes they get sorted by `buffer` and `begin` to allow overlapping
       ranges to be merged together. While it would be *theoretically* enough
       to sort by just `begin` to discover overlapping ranges, the buffers can
       get allocated in arbitrary order, causing the output to not be
       deterministic. By sorting by the `buffer` index first we ensure
       consistent ordering regardless of allocator used or the level of memory
       fragmentation. */
    struct BufferRange {
        UnsignedInt attribute;
        UnsignedInt buffer;
        const char *begin, *end;
    };
    Containers::Array<BufferRange> bufferRanges{NoInit, uniqueAttributeCount};

    /* Gather all (whitelisted) attributes and the total buffer range spanning
       them */
    UnsignedInt vertexCount = 0;
    UnsignedInt attributeId = 0;
    UnsignedInt jointIdAttributeCount = 0;
    UnsignedInt weightAttributeCount = 0;
    Containers::Pair<Containers::StringView, Int> lastNumberedAttribute;
    /* Cannot be NoInit because that would use a custom deleter which is
       disallowed to avoid dangling function pointer call after the plugin is
       unloaded :( */
    Containers::Array<MeshAttributeData> attributeData{uniqueAttributeCount};
    #ifdef MAGNUM_BUILD_DEPRECATED
    UnsignedInt compatibilitySkinningAttributeCount = 0;
    #endif
    for(const Attribute& attribute: uniqueAttributeOrder) {
        /* Extract base name and number from builtin glTF numbered attributes,
           use the whole name otherwise */
        Containers::StringView baseAttributeName;
        if(isBuiltinNumberedMeshAttribute(attribute.name)) {
            const Containers::Array3<Containers::StringView> attributeNameNumber = attribute.name.partition('_');
            /* Numbered attributes are expected to be contiguous (COLORS_0,
               COLORS_1...). If not, print a warning, because in the MeshData
               they will appear as contiguous. */
            if(lastNumberedAttribute.first() != attributeNameNumber[0])
                lastNumberedAttribute.second() = -1;
            const Int index = attributeNameNumber[2][0] - '0';
            if(index != lastNumberedAttribute.second() + 1 && !(flags() & ImporterFlag::Quiet)) {
                Warning{} << "Trade::GltfImporter::mesh(): found attribute" << attribute.name << "but expected" << attributeNameNumber[0] << Debug::nospace << "_" << Debug::nospace << lastNumberedAttribute.second() + 1;
            }

            baseAttributeName = attributeNameNumber[0];
            lastNumberedAttribute = {baseAttributeName, index};

        /* If not a builtin glTF numbered attribute or it was something strange
           such as TEXCOORD alone, TEXCOORD_SECOND, or currently also
           TEXCOORD_10, use the full attribute string. */
        } else {
            baseAttributeName = attribute.name;
            lastNumberedAttribute = {};
        }

        /* Get the accessor view */
        Containers::Optional<Accessor> accessor = parseAccessor("Trade::GltfImporter::mesh():", attribute.value.asUnsignedInt());
        if(!accessor)
            return {};

        /* From the builtin attributes can fire either for ObjectId or for
           JointIds */
        if(configuration().value<bool>("strict") && vertexFormatComponentFormat(accessor->format) == VertexFormat::UnsignedInt) {
            /** @todo for JOINTS this prints Vector4ui while the actual
                imported attribute is then UnsignedInt[], fix this somehow? */
            Error{} << "Trade::GltfImporter::mesh(): strict mode enabled, disallowing" << attribute.name << "with a 32-bit integer vertex format" << Debug::packed << accessor->format;
            return {};
        }

        /* The MeshData class doesn't allow JointIds, Weights or ObjectId to be
           a morph target as JointIds and ObjectId are integer formats (which
           are hard/impossible to interpolate), and Weights cannot appear
           without a matching JointIds. The glTF spec also disallows those in
           morph targets, so this restriction shouldn't be a problem in
           practice. Users can always switch to custom attributes if they need
           (those aren't restricted), worst case this code gets changed to
           import them as custom, like is done for example with non-normalized
           integer colors. */
        if(attribute.morphTargetId != -1 &&
          (baseAttributeName == "JOINTS"_s ||
           baseAttributeName == "WEIGHTS"_s ||
           attribute.name == configuration().value<Containers::StringView>("objectIdAttribute"))
        ) {
            Error e;
            e << "Trade::GltfImporter::mesh():";
            if(attribute.name == configuration().value<Containers::StringView>("objectIdAttribute"))
                e << "object ID attribute";
            e << attribute.name << "is not allowed to be a morph target";
            return {};
        }

        /* Whitelist supported attribute and format combinations. If not
           allowed, name stays empty, which produces an error in a single place
           below. */
        MeshAttribute name{};
        UnsignedShort arraySize = 0;
        if(baseAttributeName == "POSITION"_s) {
            if(accessor->format == VertexFormat::Vector3 ||
               accessor->format == VertexFormat::Vector3b ||
               accessor->format == VertexFormat::Vector3bNormalized ||
               accessor->format == VertexFormat::Vector3ub ||
               accessor->format == VertexFormat::Vector3ubNormalized ||
               accessor->format == VertexFormat::Vector3s ||
               accessor->format == VertexFormat::Vector3sNormalized ||
               accessor->format == VertexFormat::Vector3us ||
               accessor->format == VertexFormat::Vector3usNormalized)
                name = MeshAttribute::Position;
        } else if(baseAttributeName == "NORMAL"_s) {
            if(accessor->format == VertexFormat::Vector3 ||
               accessor->format == VertexFormat::Vector3bNormalized ||
               accessor->format == VertexFormat::Vector3sNormalized)
                name = MeshAttribute::Normal;
        } else if(baseAttributeName == "TANGENT"_s) {
            if(accessor->format == VertexFormat::Vector4 ||
               accessor->format == VertexFormat::Vector4bNormalized ||
               accessor->format == VertexFormat::Vector4sNormalized)
                name = MeshAttribute::Tangent;
        } else if(baseAttributeName == "TEXCOORD"_s) {
            if(accessor->format == VertexFormat::Vector2 ||
               accessor->format == VertexFormat::Vector2b ||
               accessor->format == VertexFormat::Vector2bNormalized ||
               accessor->format == VertexFormat::Vector2ub ||
               accessor->format == VertexFormat::Vector2ubNormalized ||
               accessor->format == VertexFormat::Vector2s ||
               accessor->format == VertexFormat::Vector2sNormalized ||
               accessor->format == VertexFormat::Vector2us ||
               accessor->format == VertexFormat::Vector2usNormalized)
                name = MeshAttribute::TextureCoordinates;
        } else if(baseAttributeName == "COLOR"_s) {
            if(accessor->format == VertexFormat::Vector3 ||
               accessor->format == VertexFormat::Vector4 ||
               accessor->format == VertexFormat::Vector3ubNormalized ||
               accessor->format == VertexFormat::Vector4ubNormalized ||
               accessor->format == VertexFormat::Vector3usNormalized ||
               accessor->format == VertexFormat::Vector4usNormalized)
                name = MeshAttribute::Color;
        /* Joints and weights are represented as an array attribute, so the
           vertex format gets changed to a component format and the component
           count becomes the array size. */
        /** @todo consider merging JOINTS_0, JOINTS_1 etc if they follow each
            other and have the same type */
        } else if(baseAttributeName == "JOINTS"_s) {
            if(accessor->format == VertexFormat::Vector4ui ||
               accessor->format == VertexFormat::Vector4us ||
               accessor->format == VertexFormat::Vector4ub)
            {
                ++jointIdAttributeCount;
                name = MeshAttribute::JointIds;
                arraySize = vertexFormatComponentCount(accessor->format);
                accessor->format = vertexFormatComponentFormat(accessor->format);
            }
        } else if(baseAttributeName == "WEIGHTS"_s) {
            if(accessor->format == VertexFormat::Vector4 ||
               accessor->format == VertexFormat::Vector4ubNormalized ||
               accessor->format == VertexFormat::Vector4usNormalized)
            {
                ++weightAttributeCount;
                name = MeshAttribute::Weights;
                arraySize = vertexFormatComponentCount(accessor->format);
                /* vertexFormatComponentFormat() strips the normalized bit from
                   the format, need to go through the full vertexFormat()
                   composer instead */
                accessor->format = vertexFormat(accessor->format, 1, isVertexFormatNormalized(accessor->format));
            }
        /* Object ID, name custom. To avoid confusion, print the error together
           with saying it's an object ID attribute */
        } else if(attribute.name == configuration().value<Containers::StringView>("objectIdAttribute")) {
            if(accessor->format == VertexFormat::UnsignedInt ||
               accessor->format == VertexFormat::UnsignedShort ||
               accessor->format == VertexFormat::UnsignedByte)
            {
                name = MeshAttribute::ObjectId;
            }

        /* Custom or unrecognized attributes, map to an ID. Any type is
           allowed. */
        } else {
            name = _d->meshAttributesForName.at(attribute.name);
        }

        /* A type that's not valid for given builtin attribute */
        if(name == MeshAttribute{}) {
            /* In strict mode, print an error. Otherwise print a warning unless
               quiet output is requested. */
            if(configuration().value<bool>("strict") || !(flags() & ImporterFlag::Quiet)) {
                Debug e = configuration().value<bool>("strict") ? static_cast<Debug&&>(Error{}) : static_cast<Debug&&>(Warning{});
                e << "Trade::GltfImporter::mesh(): unsupported";

                /* If the attribute is meant to be recognized as an object ID,
                   mention it as such to avoid confusion */
                if(attribute.name == configuration().value<Containers::StringView>("objectIdAttribute"))
                    e << "object ID attribute";

                /* Here the VertexFormat prefix would not be confusing but
                   print it without to be consistent with other messages */
                e << attribute.name << "format" << Debug::packed << accessor->format;

                /* Indicate it's invalid for a morph target */
                if(attribute.morphTargetId != -1)
                    e << "in morph target" << attribute.morphTargetId;

                if(configuration().value<bool>("strict"))
                    e << Debug::nospace << ", set strict=false to import as a custom attribute";
                else
                    e << Debug::nospace << ", importing as a custom attribute";
            }

            /* In strict mode this is a failure. Otherwise we import it as a
               custom attribute. The meshAttributesForName contains all
               attribute names found in the file, including the builtin
               ones. */
            if(configuration().value<bool>("strict"))
                return {};
            else
                name = _d->meshAttributesForName.at(attribute.name);
        }

        /* Remember vertex count and ensure all accessors have the same */
        if(attributeId == 0) {
            vertexCount = accessor->data.size()[0];
        } else if(accessor->data.size()[0] != vertexCount) {
            Error e;
            e << "Trade::GltfImporter::mesh(): mismatched vertex count for attribute" << attribute.name;
            if(attribute.morphTargetId != -1)
                e << "in morph target" << attribute.morphTargetId;
            e << Debug::nospace << ", expected" << vertexCount << "but got" << accessor->data.size()[0];
            return {};
        }

        /* If the accessor has no backing view or is sparse, use a special
           buffer ID that puts it at the very end when sorted below, i.e. not
           overlapping with any other buffer range. Make the attribute view
           contiguous and exactly matching the range because the range is going
           to be either zero-filled or copied and then patched, or both.

           Non-sparse zero-filled ranges could overlap each other to reduce
           memory use (or even be just a single element with zero stride) but
           I'm skeptical of practical value of all-zero attributes so they
           don't -- if your mesh has them, you're going to pay for the extra
           waste.

           Similarly, it could happen that the same sparse accessor is used for
           multiple attributes, and this duplicates it. Again I don't think
           there's much practical value in that so no attempt at deduplicating
           / overlapping those is made. Conversely, it could happen that a
           sparse attribute uses is interleaved with others, and the original
           non-sparse location isn't used by anything else. Which means the
           original non-sparse location stays in the mesh, unused, with the
           sparse attribute being copied next to it. *Again*, I don't think
           that's a real-world use case, because likely there will be e.g. a
           position and then a morph target position (or more of them),
           changing some parts of the original, thus always with the original
           used as well. And if not, feel free to use MeshTools::interleave()
           to get rid of the unused data.  */
        Containers::StridedArrayView2D<const char> data;
        if(accessor->bufferView == ~UnsignedInt{} || accessor->sparseValues.data()) {
            /* The pointer is nullptr to indicate there's no buffer view to
               directly copy from -- even if there would be, it likely has to
               get deinterleaved */
            bufferRanges[attributeId] = {
                attributeId,
                ~UnsignedInt{},
                nullptr,
                reinterpret_cast<const char*>(vertexCount*accessor->data.size()[1])
            };
            /* The attribute data view then matches the buffer range exactly */
            data = Containers::StridedArrayView2D<const char>{{nullptr, vertexCount*accessor->data.size()[1]}, {vertexCount, accessor->data.size()[1]}};

        /* For non-sparse accessors backed by buffer views remember the buffer
           index and memory range the attribute is in. Cannot take just the
           whole buffer view range because the accessor could be a tiny slice
           of that (which is the case for example with the Buggy model in glTF
           Sample Assets), thus have to take exactly the range the accessor
           spans.*/
        } else {
            const char* const attributeDataBegin = static_cast<const char*>(accessor->data.data());
            bufferRanges[attributeId] = {
                attributeId,
                _d->bufferViews[accessor->bufferView]->buffer,
                attributeDataBegin,
                /* Unless there are no vertices, from the last vertex we take
                   just the size the actual format spans, which is provided in
                   the second dimension of the data view */
                attributeDataBegin + (vertexCount ? (vertexCount - 1)*accessor->data.stride()[0] + accessor->data.size()[1] : 0),
            };
            data = accessor->data;
        }

        /** @todo Check that accessor stride >= vertexFormatSize(format)? */

        /* Fill in an attribute. Points to the input data, will be patched to
           the output data once we know where it's allocated. */
        attributeData[attributeId] = MeshAttributeData{name, accessor->format, data, arraySize, attribute.morphTargetId};

        /* For backwards compatibility we'll insert also a custom "JOINTS" /
           "WEIGHTS" attributes below, count how many of them we'll need */
        #ifdef MAGNUM_BUILD_DEPRECATED
        if((name == MeshAttribute::JointIds || name == MeshAttribute::Weights) && configuration().value<bool>("compatibilitySkinningAttributes"))
            ++compatibilitySkinningAttributeCount;
        #endif

        ++attributeId;
    }

    /* Verify we really filled all attributes */
    CORRADE_INTERNAL_ASSERT(attributeId == attributeData.size());

    /* 3.7.2.1 (Geometry § Meshes § Overview) says "[count] MUST be non-zero",
       but we allow also none unless the strict option is enabled. Not printing
       a warning if the strict option is disabled as Magnum can handle the
       vertex-less MeshData just fine. */
    if(!vertexCount && configuration().value<bool>("strict")) {
        Error{} << "Trade::GltfImporter::mesh(): strict mode enabled, disallowing a mesh with no vertices";
        return {};
    }

    /* 3.7.3.3 (Geometry § Skins § Skinned mesh attributes) says "For a given
       primitive, the number of JOINTS_n attribute sets MUST be equal to the
       number of WEIGHTS_n attribute sets". Which aligns well with the
       assertion that's in MeshData itself. */
    if(jointIdAttributeCount != weightAttributeCount) {
        Error{} << "Trade::GltfImporter::mesh(): the mesh has" << jointIdAttributeCount << "JOINTS_n attributes but" << weightAttributeCount << "WEIGHTS_n attributes";
        return {};
    }

    /* Sort buffer ranges by their begin pointer, which then allows merging
       ones that overlap in a single pass afterwards. No need to use
       std::stable_sort() because if two accessors begin at the same memory,
       both will get the same offset in the output buffer, no matter the
       order.

       To avoid different results depending on where the individual buffers
       were allocated in memory, sort by buffer IDs first. This is verified in
       the meshBuffers() test by explicitly importing particular buffers in
       different order, causing their mutual allocation order to differ. */
    std::sort(bufferRanges.begin(), bufferRanges.end(), [](const BufferRange& a, const BufferRange& b) {
        /* If the buffer differs, sort by that */
        if(a.buffer != b.buffer)
            return a.buffer < b.buffer;
        /* If the base pointer differs within the buffer, sort by that */
        if(a.begin != b.begin)
            return a.begin < b.begin;
        /* Otherwise, for accessors backed by buffer views, the order doesn't
           matter -- they'll get merged together below so both will end up at
           the same offset in the output vertexData, no matter how the
           particular std::sort() implementation works. However, accessors not
           backed by buffer views (i.e., all having `buffer` ~UnsignedInt{} and
           `begin` nullptr) should still preserve some deterministic order, so
           sort them by the attribute ID. */
        return a.attribute < b.attribute;
    });

    /* Go through the sorted ranges and merge ones that either just touch (for
       example when one attribute is right after the one before) or overlap
       (for example when two or more attributes are interleaved together). */
    std::size_t inputRange = 0;
    for(std::size_t i = 1; i < bufferRanges.size(); ++i) {
        /* Stop once we reach sparse or zero-filled buffer ranges, which are
           sorted at the end. Those are not meant to be merged in any way as
           their contents get runtime-generated, not copied from anywhere. */
        if(bufferRanges[i].buffer == ~UnsignedInt{})
            break;

        /* If the buffer is different (which means the two buffer ranges may be
           in any order relative to each other) or the input range ends before
           this one, nothing gets merged, and next iteration we'll compare to
           this range instead */
        if(bufferRanges[inputRange].buffer != bufferRanges[i].buffer ||
           bufferRanges[inputRange].end < bufferRanges[i].begin) {
            inputRange = i;

        /* If the input range end is exactly at this range begin or further,
           merge the two. Mark this range as merged to the previous by setting
           its `end` to nullptr. A max() is used because the original range may
           still extend beyond this one, verified in one of the cases in the
           meshBuffers() test. */
        } else {
            bufferRanges[inputRange].end = Utility::max(bufferRanges[inputRange].end, bufferRanges[i].end);
            bufferRanges[i].end = nullptr;
        }
    }

    /* At this point, entries in bufferRanges that don't have end offset all 1s
       are all unique & mutually non-overlapping. Calculate the total size,
       with each range aligned to four bytes, allocate an array for them and
       then copy the vertex data there in a second pass. */
    std::size_t vertexDataSize = 0;
    for(const BufferRange& i: bufferRanges) {
        /* The second pass below checks also for `end == begin` in case `end`
           is nullptr, but in that case the range is empty and wouldn't add
           anything to vertexDataSize, so we can skip that too */
        if(!i.end)
            continue;
        /* Align to four bytes */
        vertexDataSize += 4*((i.end - i.begin + 3)/4);
    }
    Containers::Array<char> vertexData{NoInit, vertexDataSize};
    std::size_t rangeToCopyFrom = ~std::size_t{};
    std::size_t vertexDataOffset = 0;
    for(std::size_t i = 0; i != bufferRanges.size(); ++i) {
        /* If this range wasn't merged to an earlier one, signalized by `end`
           being nullptr, copy its data. In case of empty meshes the `begin`
           can be nullptr as well, that's not a merged range. */
        if(bufferRanges[i].end || bufferRanges[i].end == bufferRanges[i].begin) {
            if(i != 0)
                /* Align to four bytes, matching the above */
                vertexDataOffset += 4*((bufferRanges[rangeToCopyFrom].end - bufferRanges[rangeToCopyFrom].begin + 3)/4);
            const std::size_t size = bufferRanges[i].end - bufferRanges[i].begin;

            /* If the attribute has no backing buffer view, zero-init its
               memory */
            if(!_d->accessors[uniqueAttributeOrder[bufferRanges[i].attribute].value.asUnsignedInt()]->data.data())
                std::memset(vertexData.sliceSize(vertexDataOffset, size), 0, size);

            /* Otherwise, if it isn't sparse, signalled by `begin` being null,
               copy its contents. Sparse attributes need to get their buffer
               contents deinterleaved first, which is done together with sparse
               patching below. */
            else if(bufferRanges[i].begin) Utility::copy(
                Containers::arrayView(bufferRanges[i].begin, size),
                vertexData.sliceSize(vertexDataOffset, size));

            /* Zero-fill the extra bytes in case the range will get padded.
               Assuming large meshes consisting of rather few buffer ranges
               this is faster than allocating `vertexData` with ValueInit. */
            const std::size_t alignedSize = 4*((size + 3)/4);
            for(std::size_t i = size; i != alignedSize; ++i)
                vertexData[vertexDataOffset + i] = '\0';

            rangeToCopyFrom = i;
        }

        /* The MeshAttributeData corresponding to this range was initialized
           with a view directly on the input buffer. Redirect it to point to
           the vertexData array. */
        MeshAttributeData& attribute = attributeData[bufferRanges[i].attribute];
        attribute = MeshAttributeData{
            attribute.name(),
            attribute.format(),
            Containers::StridedArrayView1D<char>{
                /* glTF only requires buffer views to be large enough to fit
                   the actual data, not to have the size large enough to fit
                   `count*stride` elements. The StridedArrayView expects the
                   latter, so we fake the vertexData size to satisfy the
                   assert. For simplicity we overextend by the whole stride
                   instead of `offset + typeSize`, relying on parseAccessor()
                   having checked the bounds already (and there is a similar
                   workaround when populating the output view). */
                /** @todo instead of faking the size, split the offset into
                    offset in whole strides and the remainder (Math::div), then
                    form the view with offset in whole strides and then "shift"
                    the view by the remainder (once there's
                    StridedArrayView::shift() or some such) */
                {vertexData, vertexData.size() + attribute.stride()},
                /* The input buffer range we is starting at
                   vertexData[vertexDataOffset] ... */
                vertexData + vertexDataOffset +
                    /* ... the attribute is then at an offset that's a
                       difference between beginning of the range we copied from
                       and the actual attribute data pointer. In case of
                       accessors that are sparse or have no backing buffer
                       views, both of these are nullptr so they don't change
                       the offset in any way -- they always start at the range
                       begin. */
                    (static_cast<const char*>(attribute.data().data()) - bufferRanges[rangeToCopyFrom].begin),
                vertexCount, attribute.stride()},
            attribute.arraySize(),
            attribute.morphTargetId()};
    }

    /* Verify we copied everything to the correct offsets. The vertexDataOffset
       should contain everything except the last range, which is contained in
       rangeToCopyFrom */
    CORRADE_INTERNAL_ASSERT(bufferRanges.isEmpty() || vertexDataOffset + 4*(std::size_t(bufferRanges[rangeToCopyFrom].end - bufferRanges[rangeToCopyFrom].begin + 3)/4) == vertexDataSize);

    /* Fill in sparse accessors. The original uniqueAttributeOrder contains
       accessor IDs which we can use to decide whether the accessor is sparse
       or not. */
    CORRADE_INTERNAL_ASSERT(attributeData.size() == uniqueAttributeOrder.size());
    for(std::size_t i = 0; i != attributeData.size(); ++i) {
        const Accessor& accessor = *_d->accessors[uniqueAttributeOrder[i].value.asUnsignedInt()];
        if(!accessor.sparseValues.data())
            continue;

        /* Turn the attribute view mutable. See a similar case above for why
           the extra stride is added. */
        /** @todo some arrayConstCast, ugh? */
        Containers::StridedArrayView2D<char> data{{vertexData, vertexData.size() + attributeData[i].stride()},
            const_cast<char*>(static_cast<const char*>(attributeData[i].data().data())),
            /* Reuse type size info from the accessor to avoid a lookup in
               vertexFormatSize() */
            {attributeData[i].data().size(), accessor.data.size()[1]},
            {attributeData[i].stride(), 1}};

        /* Copy & deinterleave the original attribute data, if the accessor has
           a backing buffer view. This wasn't done in the loop above because
           there it only copies the contents without changing the layout. */
        if(accessor.data.data())
            Utility::copy(accessor.data, data);

        /* The copy operation also checks the index values and prints a message
           if they're out of range. Fail in that case. */
        bool success;
        if(accessor.sparseIndices.size()[1] == 4)
            success = applySparseAccessor("Trade::GltfImporter::mesh():", uniqueAttributeOrder[i].value.asUnsignedInt(), Containers::arrayCast<1, const UnsignedInt>(accessor.sparseIndices), accessor.sparseValues, data);
        else if(accessor.sparseIndices.size()[1] == 2)
            success = applySparseAccessor("Trade::GltfImporter::mesh():", uniqueAttributeOrder[i].value.asUnsignedInt(), Containers::arrayCast<1, const UnsignedShort>(accessor.sparseIndices), accessor.sparseValues, data);
        else if(accessor.sparseIndices.size()[1] == 1)
            success = applySparseAccessor("Trade::GltfImporter::mesh():", uniqueAttributeOrder[i].value.asUnsignedInt(), Containers::arrayCast<1, const UnsignedByte>(accessor.sparseIndices), accessor.sparseValues, data);
        else CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
        if(!success)
            return {};
    }

    /* For backwards compatibility insert custom "JOINTS" and "WEIGHTS"
       attributes which are a Vector4<T> instead of T[4]. The attribute array
       has to remain with the default deleter, so allocate a new one with extra
       space and then put them at the end.

       This is done only after the attributes were patched to point to the
       output `vertexData` above, and after sparse accessors were applied so we
       can just reuse that patched view instead of having to patch it twice,
       or have sparse accessors applied more than once for no reason. */
    #ifdef MAGNUM_BUILD_DEPRECATED
    if(compatibilitySkinningAttributeCount) {
        /* Again cannot be NoInit because that would use a custom deleter which
           is disallowed to avoid dangling function pointer call after the
           plugin is unloaded */
        Containers::Array<MeshAttributeData> compatibilityAttributeData{attributeData.size() + compatibilitySkinningAttributeCount};
        Utility::copy(attributeData, compatibilityAttributeData.prefix(attributeData.size()));

        for(const MeshAttributeData& attribute: attributeData) {
            MeshAttribute name;
            if(attribute.name() == MeshAttribute::JointIds)
                name = _d->meshAttributesForName.at("JOINTS"_s);
            else if(attribute.name() == MeshAttribute::Weights)
                name = _d->meshAttributesForName.at("WEIGHTS"_s);
            else continue;

            /* The builtin attribute name is replaced with a custom one, format
               array size becomes number of vector components */
            compatibilityAttributeData[attributeId] = MeshAttributeData{name, vertexFormat(attribute.format(), attribute.arraySize(), isVertexFormatNormalized(attribute.format())), attribute.data(), 0, attribute.morphTargetId()};
            ++attributeId;
        }

        /* Replace the attribute data array with the new one containing also
           the compatibility attributes */
        Utility::swap(attributeData, compatibilityAttributeData);
    }

    /* Verify we really filled all attributes, including the compatibility ones
       at the end */
    CORRADE_INTERNAL_ASSERT(attributeId == attributeData.size());
    #endif

    /* Flip Y axis of texture coordinates, unless it's done in the material
       instead */
    for(std::size_t i = 0; i != attributeData.size(); ++i) {
        if(attributeData[i].name() != MeshAttribute::TextureCoordinates || _d->textureCoordinateYFlipInMaterial)
            continue;

        /* Turn the attribute view mutable. See a similar case above for why
           the extra stride is added. */
        /** @todo some arrayConstCast, ugh? */
        Containers::StridedArrayView1D<char> data{{vertexData, vertexData.size() + attributeData[i].stride()},
            const_cast<char*>(static_cast<const char*>(attributeData[i].data().data())),
            attributeData[i].data().size(),
            attributeData[i].stride()};

        if(attributeData[i].format() == VertexFormat::Vector2)
            for(auto& c: Containers::arrayCast<Vector2>(data))
                c.y() = 1.0f - c.y();
        else if(attributeData[i].format() == VertexFormat::Vector2ubNormalized)
            for(auto& c: Containers::arrayCast<Vector2ub>(data))
                c.y() = 255 - c.y();
        else if(attributeData[i].format() == VertexFormat::Vector2usNormalized)
            for(auto& c: Containers::arrayCast<Vector2us>(data))
                c.y() = 65535 - c.y();
        /* For these it's always done in the material texture transform as
           we can't do a 1 - y flip like above. These are allowed only by
           the KHR_mesh_quantization formats and in that case the texture
           transform should be always present. */
        /* LCOV_EXCL_START */
        else if(attributeData[i].format() != VertexFormat::Vector2bNormalized &&
                attributeData[i].format() != VertexFormat::Vector2sNormalized &&
                attributeData[i].format() != VertexFormat::Vector2ub &&
                attributeData[i].format() != VertexFormat::Vector2b &&
                attributeData[i].format() != VertexFormat::Vector2us &&
                attributeData[i].format() != VertexFormat::Vector2s)
            CORRADE_INTERNAL_ASSERT_UNREACHABLE();
        /* LCOV_EXCL_STOP */
    }

    /* Indices */
    MeshIndexData indices;
    Containers::Array<char> indexData;
    if(const Utility::JsonIterator gltfIndices = gltfPrimitive.find("indices"_s)) {
        if(!_d->gltf->parseUnsignedInt(*gltfIndices)) {
            Error{} << "Trade::GltfImporter::mesh(): invalid indices property";
            return {};
        }
        /* Bounds check is done in parseAccessor() below, no need to do it
           here again */

        Containers::Optional<Accessor> accessor = parseAccessor("Trade::GltfImporter::mesh():", gltfIndices->asUnsignedInt());
        if(!accessor)
            return {};

        /* There's no technical reason this couldn't work, it's just that I
           don't see any practical use case that would warrant the extra
           testing effort, so just fail for now */
        if(accessor->bufferView == ~UnsignedInt{}) {
            Error{} << "Trade::GltfImporter::mesh(): index accessor" << gltfIndices->asUnsignedInt() << "has no buffer view, which is unsupported";
            return {};
        }
        if(accessor->sparseValues.data()) {
            Error{} << "Trade::GltfImporter::mesh(): index accessor" << gltfIndices->asUnsignedInt() << "is using sparse storage, which is unsupported";
            return {};
        }

        MeshIndexType type;
        if(accessor->format == VertexFormat::UnsignedByte)
            type = MeshIndexType::UnsignedByte;
        else if(accessor->format == VertexFormat::UnsignedShort)
            type = MeshIndexType::UnsignedShort;
        else if(accessor->format == VertexFormat::UnsignedInt)
            type = MeshIndexType::UnsignedInt;
        else {
            /* Since we're abusing VertexFormat for all formats, print just the
               enum value without the prefix to avoid cofusion */
            Error{} << "Trade::GltfImporter::mesh(): unsupported index type" << Debug::packed << accessor->format;
            return {};
        }

        if(!accessor->data.isContiguous()) {
            Error{} << "Trade::GltfImporter::mesh(): index buffer view" << accessor->bufferView << "is not contiguous";
            return {};
        }

        Containers::ArrayView<const char> srcContiguous = accessor->data.asContiguous();
        indexData = Containers::Array<char>{NoInit, srcContiguous.size()};
        Utility::copy(srcContiguous, indexData);
        indices = MeshIndexData{type, indexData};
    }

    /* If we have an index-less attribute-less mesh, glTF has no way to supply
       a vertex count, so return 0 */
    if(!indices.data().size() && !attributeData.size())
        return MeshData{primitive, 0};

    return MeshData{primitive,
        Utility::move(indexData), indices,
        Utility::move(vertexData), Utility::move(attributeData),
        vertexCount, &gltfPrimitive.token()};
}

MeshAttribute GltfImporter::doMeshAttributeForName(const Containers::StringView name) {
    return _d ? _d->meshAttributesForName[name] : MeshAttribute{};
}

Containers::String GltfImporter::doMeshAttributeName(const MeshAttribute name) {
    return _d && meshAttributeCustom(name) < _d->meshAttributeNames.size() ?
        _d->meshAttributeNames[meshAttributeCustom(name)] : ""_s;
}

UnsignedInt GltfImporter::doMaterialCount() const {
    return _d->gltfMaterials.size();
}

Int GltfImporter::doMaterialForName(const Containers::StringView name) {
    if(!_d->materialsForName) {
        _d->materialsForName.emplace();
        _d->materialsForName->reserve(_d->gltfMaterials.size());
        for(std::size_t i = 0; i != _d->gltfMaterials.size(); ++i)
            if(const Containers::StringView n = _d->gltfMaterials[i].second())
                _d->materialsForName->emplace(n, i);
    }

    const auto found = _d->materialsForName->find(name);
    return found == _d->materialsForName->end() ? -1 : found->second;
}

Containers::String GltfImporter::doMaterialName(const UnsignedInt id) {
    return _d->gltfMaterials[id].second();
}

namespace {

/** @todo turn this into a helper API on MaterialAttributeData and then drop
    from here and AssimpImporter */
bool checkMaterialAttributeSize(const Containers::StringView name, const MaterialAttributeType type, const ImporterFlags flags, const void* const value = nullptr) {
    std::size_t valueSize;
    if(type == MaterialAttributeType::String) {
        CORRADE_INTERNAL_ASSERT(value);
        /* +2 are null byte and size */
        valueSize = static_cast<const Containers::StringView*>(value)->size() + 2;
    } else
        valueSize = materialAttributeTypeSize(type);

    /* +1 is the key null byte */
    if(valueSize + name.size() + 1 + sizeof(MaterialAttributeType) > sizeof(MaterialAttributeData)) {
        if(!(flags & ImporterFlag::Quiet))
            Warning{} << "Trade::GltfImporter::material(): property" << name << "is too large with" << valueSize + name.size() << "bytes, skipping";
        return false;
    }

    return true;
}

Containers::Optional<MaterialAttributeData> parseMaterialAttribute(Utility::Json& gltf, /*mutable*/ Containers::StringView name, const Utility::JsonToken gltfValue, const ImporterFlags flags) {
    /* The `name` is not const, gets modified if the first letter isn't
       lowercase */
    if(!name) {
        if(!(flags & ImporterFlag::Quiet))
            Warning{} << "Trade::GltfImporter::material(): property with an empty name, skipping";
        return {};
    }

    /* We only need temporary storage for parsing primitive (arrays) as bool/
       Float/Vector[2/3/4]. Other types/sizes are either converted or ignored,
       so we know the upper limit on the data size. The alignas prevents
       unaligned reads for individual floats. For strings,
       MaterialAttributeData expects a pointer to StringView. */
    alignas(4) char attributeData[16];
    Containers::StringView attributeStringView;
    MaterialAttributeType type{};

    /* Generic object, skip. Not parsing textureInfo objects here because
       they're only needed by extensions but not by extras. They may also
       append more than one attribute, so this is handled directly in the
       extension parsing loop. */
    if(gltfValue.type() == Utility::JsonToken::Type::Object) {
        if(!(flags & ImporterFlag::Quiet))
            Warning{} << "Trade::GltfImporter::material(): property" << name << "is an object, skipping";
        return {};

    /* Array, hopefully numeric */
    } else if(gltfValue.type() == Utility::JsonToken::Type::Array) {
        for(const Utility::JsonToken i: *gltf.parseArray(gltfValue)) {
            if(i.type() != Utility::JsonToken::Type::Number) {
                if(!(flags & ImporterFlag::Quiet))
                    Warning{} << "Trade::GltfImporter::material(): property" << name << "is not a numeric array, skipping";
                return {};
            }
        }

        /* Always interpret numbers as floats because the type can be
           ambiguous. E.g. integer attributes may use exponent notation and
           decimal points, making correct type detection depend on glTF
           exporter behaviour. */
        Containers::Optional<Containers::StridedArrayView1D<const float>> valueArray = gltf.parseFloatArray(gltfValue);
        /* No use importing arbitrarily-sized arrays of primitives, those are
           currently not used in any glTF extension */
        if(!valueArray || valueArray->size() < 1 || valueArray->size() > 4) {
            if(!(flags & ImporterFlag::Quiet))
                Warning{} << "Trade::GltfImporter::material(): property" << name << "is an invalid or unrepresentable numeric vector, skipping";
            return {};
        }

        constexpr MaterialAttributeType vectorType[]{
            MaterialAttributeType::Float,
            MaterialAttributeType::Vector2,
            MaterialAttributeType::Vector3,
            MaterialAttributeType::Vector4
        };
        type = vectorType[valueArray->size() - 1];
        Utility::copy(*valueArray, {reinterpret_cast<Float*>(attributeData), valueArray->size()});

    /* Null. Not sure what for, skipping. If the token is not actually a valid
       null value, the error gets silently ignored. */
    } else if(gltfValue.type() == Utility::JsonToken::Type::Null) {
        if(!(flags & ImporterFlag::Quiet))
            Warning{} << "Trade::GltfImporter::material(): property" << name << "is a null, skipping";
        return {};

    /* Bool */
    } else if(gltfValue.type() == Utility::JsonToken::Type::Bool) {
        Containers::Optional<bool> b;
        {
            /* Redirect error messages from Json::parse*() to the warning
               output as they are non-fatal and only lead to given attribute
               being skipped. If quiet output is requested, don't print them at
               all. */
            Error redirectError{flags & ImporterFlag::Quiet ? nullptr : Warning::output()};
            b = gltf.parseBool(gltfValue);
        }
        if(b) {
            type = MaterialAttributeType::Bool;
            *reinterpret_cast<bool*>(attributeData) = *b;
        } else {
            if(!(flags & ImporterFlag::Quiet))
                Warning{} << "Trade::GltfImporter::material(): property" << name << "is invalid, skipping";
            return {};
        }

    /* Number */
    } else if(gltfValue.type() == Utility::JsonToken::Type::Number) {
        /* Always interpret numbers as floats because the type can be
           ambiguous. E.g. integer attributes may use exponent notation and
           decimal points, making correct type detection depend on glTF
           exporter behaviour. */
        Containers::Optional<Float> f;
        {
            /* Redirect error messages from Json::parse*() to the warning
               output as they are non-fatal and only lead to given attribute
               being skipped. If quiet output is requested, don't print them at
               all. */
            Error redirectError{flags & ImporterFlag::Quiet ? nullptr : Warning::output()};
            f = gltf.parseFloat(gltfValue);
        }
        if(f) {
            type = MaterialAttributeType::Float;
            *reinterpret_cast<Float*>(attributeData) = *f;
        } else {
            if(!(flags & ImporterFlag::Quiet))
                Warning{} << "Trade::GltfImporter::material(): property" << name << "is invalid, skipping";
            return {};
        }

    /* String */
    } else if(gltfValue.type() == Utility::JsonToken::Type::String) {
        Containers::Optional<Containers::StringView> s;
        {
            /* Redirect error messages from Json::parse*() to the warning
               output as they are non-fatal and only lead to given attribute
               being skipped. If quiet output is requested, don't print them at
               all. */
            Error redirectError{flags & ImporterFlag::Quiet ? nullptr : Warning::output()};
            s = gltf.parseString(gltfValue);
        }
        if(s) {
            type = MaterialAttributeType::String;
            attributeStringView = *s;
        } else {
            if(!(flags & ImporterFlag::Quiet))
                Warning{} << "Trade::GltfImporter::material(): property" << name << "is invalid, skipping";
            return {};
        }

    /* No other token types exist */
    } else CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */

    CORRADE_INTERNAL_ASSERT(type != MaterialAttributeType{});

    const void* const valuePointer = type == MaterialAttributeType::String ?
        static_cast<const void*>(&attributeStringView) : static_cast<const void*>(attributeData);
    if(!checkMaterialAttributeSize(name, type, flags, valuePointer))
        return {};

    /* Uppercase attribute names are reserved. Standard glTF (extension)
       attributes should all be lowercase but we don't have this guarantee for
       extras attributes. Can't use String::nullTerminatedView() here because
       JSON tokens are not null-terminated. */
    Containers::String nameLowercase;
    if(std::isupper(static_cast<unsigned char>(name.front()))) {
        nameLowercase = name;
        nameLowercase.front() = std::tolower(static_cast<unsigned char>(name.front()));
        name = nameLowercase;
    }

    return MaterialAttributeData{name, type, valuePointer};
}

}

/* In this case, extra attributes such as Matrix or Coordinates are prefixed
   with the attribute */
bool GltfImporter::materialTexture(const Utility::JsonToken gltfTexture, Containers::Array<MaterialAttributeData>& attributes, const Containers::StringView attribute, bool warningOnly) {
    CORRADE_INTERNAL_ASSERT(attribute);
    return materialTexture(gltfTexture, attributes, attribute, attribute, warningOnly);
}

/* The attribute can be empty in case of adding e.g. a combined
   SpecularGlossinessTexture but with separate SpecularTextureMatrix and
   GlossinessTextureMatrix. Then the first call gets
   `attribute=SpecularGlossinessTexture` and
   `extraAttributePrefix=SpecularTexture` and s second call is `attribute=` and
   `extraAttributePrefix=GlossinessTexture`. */
bool GltfImporter::materialTexture(const Utility::JsonToken gltfTexture, Containers::Array<MaterialAttributeData>& attributes, const Containers::StringView attribute, const Containers::StringView extraAttributePrefix, bool warningOnly) {
    if(!_d->gltf->parseObject(gltfTexture)) {
        if(!warningOnly || !(flags() & ImporterFlag::Quiet))
            (warningOnly ? static_cast<Debug&&>(Warning{}) : static_cast<Debug&&>(Error{})) << "Trade::GltfImporter::material(): invalid" << gltfTexture.parent()->asString() << "property";
        return false;
    }

    const Utility::JsonIterator gltfIndex = gltfTexture.find("index"_s);
    if(!gltfIndex || !_d->gltf->parseUnsignedInt(*gltfIndex)) {
        if(!warningOnly || !(flags() & ImporterFlag::Quiet))
            (warningOnly ? static_cast<Debug&&>(Warning{}) : static_cast<Debug&&>(Error{})) << "Trade::GltfImporter::material(): missing or invalid" << gltfTexture.parent()->asString() << "index property";
        return false;
    }
    if(gltfIndex->asUnsignedInt() >= _d->gltfTextures.size()) {
        /* This reports the range in original glTF textures instead of the
           potentially deduplicated KHR_texture_ktx textures. That should be
           fine, since fixing the error will happen on the input file, before
           any deduplication. */
        if(!warningOnly || !(flags() & ImporterFlag::Quiet))
            (warningOnly ? static_cast<Debug&&>(Warning{}) : static_cast<Debug&&>(Error{})) << "Trade::GltfImporter::material():" << gltfTexture.parent()->asString() << "index" << gltfIndex->asUnsignedInt() << "out of range for" << _d->gltfTextures.size() << "textures";
        return false;
    }
    const UnsignedInt uniqueIndex = _d->uniqueTextureForGltfTexture[gltfIndex->asUnsignedInt()];

    /** @todo avoid allocations when it's doable in a less error-prone way than
        formatInto() + slice() (ArrayTuple string support?), best with a
        statically-sized buffer */
    CORRADE_INTERNAL_ASSERT(extraAttributePrefix);
    const Containers::String matrixAttribute = extraAttributePrefix + "Matrix"_s;
    const Containers::String coordinateAttribute = extraAttributePrefix + "Coordinates"_s;
    const Containers::String layerAttribute = extraAttributePrefix + "Layer"_s;

    if(configuration().value<bool>("experimentalKhrTextureKtx")) {
        const Utility::JsonIterator gltfTextureExtensions = Utility::JsonToken{*_d->gltf, _d->gltfTextures[gltfIndex->asUnsignedInt()].first()}.find("extensions"_s);
        Utility::JsonIterator gltfKhrTextureKtx;
        Utility::JsonIterator gltfTextureLayer;
        if(gltfTextureExtensions &&
           (gltfKhrTextureKtx = gltfTextureExtensions->find("KHR_texture_ktx"_s)) &&
           (gltfTextureLayer = gltfKhrTextureKtx->find("layer"_s)))
        {
            if(!_d->gltf->parseUnsignedInt(*gltfTextureLayer)) {
                if(!warningOnly || !(flags() & ImporterFlag::Quiet))
                    (warningOnly ? static_cast<Debug&&>(Warning{}) : static_cast<Debug&&>(Error{})) << "Trade::GltfImporter::material(): invalid KHR_texture_ktx layer property";
                return false;
            }

            if(checkMaterialAttributeSize(layerAttribute, MaterialAttributeType::UnsignedInt, flags()))
                arrayAppend(attributes, InPlaceInit, layerAttribute, gltfTextureLayer->asUnsignedInt());
        }
    }

    /* Texture coordinate is optional, defaults to 0. Include it if present in
       the file regardless of whether it's default nor not, consistently with
       other material attributes. */
    Containers::Optional<UnsignedInt> texCoord;
    if(const Utility::JsonIterator gltfTexCoord = gltfTexture.find("texCoord"_s)) {
        if(!_d->gltf->parseUnsignedInt(*gltfTexCoord)) {
            if(!warningOnly || !(flags() & ImporterFlag::Quiet))
                (warningOnly ? static_cast<Debug&&>(Warning{}) : static_cast<Debug&&>(Error{})) << "Trade::GltfImporter::material(): invalid" << gltfTexture.parent()->asString() << "texcoord property";
            return false;
        }

        texCoord = gltfTexCoord->asUnsignedInt();
    }

    /* Extensions */
    Utility::JsonIterator gltfKhrTextureTransform;
    if(const Utility::JsonIterator gltfExtensions = gltfTexture.find("extensions"_s)) {
        if(!_d->gltf->parseObject(*gltfExtensions)) {
            if(!warningOnly || !(flags() & ImporterFlag::Quiet))
                (warningOnly ? static_cast<Debug&&>(Warning{}) : static_cast<Debug&&>(Error{})) << "Trade::GltfImporter::material(): invalid" << gltfTexture.parent()->asString() << "extensions property";
            return false;
        }

        /* Texture transform. Because texture coordinates were Y-flipped, we
           first unflip them back, apply the transform (which assumes origin at
           bottom left and Y down) and then flip the result again. Sanity of
           the following verified with https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/TextureTransformTest */
        gltfKhrTextureTransform = gltfExtensions->find("KHR_texture_transform"_s);
        if(gltfKhrTextureTransform && !_d->gltf->parseObject(*gltfKhrTextureTransform)) {
            if(!warningOnly || !(flags() & ImporterFlag::Quiet))
                (warningOnly ? static_cast<Debug&&>(Warning{}) : static_cast<Debug&&>(Error{})) << "Trade::GltfImporter::material(): invalid" << gltfTexture.parent()->asString() << "KHR_texture_transform extension";
            return false;
        }
        if(gltfKhrTextureTransform && checkMaterialAttributeSize(matrixAttribute, MaterialAttributeType::Matrix3x3, flags())) {
            Matrix3 matrix;

            /* If material needs an Y-flip, the mesh doesn't have the texture
               coordinates flipped and thus we don't need to unflip them
               first */
            if(!_d->textureCoordinateYFlipInMaterial)
                matrix = Matrix3::translation(Vector2::yAxis(1.0f))*
                         Matrix3::scaling(Vector2::yScale(-1.0f));

            /* The extension can override texture coordinate index (for example
               to have the unextended coordinates already transformed, and
               applying transformation to a different set) */
            if(const Utility::JsonIterator gltfTexCoord = gltfKhrTextureTransform->find("texCoord"_s)) {
                if(!_d->gltf->parseUnsignedInt(*gltfTexCoord)) {
                    if(!warningOnly || !(flags() & ImporterFlag::Quiet))
                        (warningOnly ? static_cast<Debug&&>(Warning{}) : static_cast<Debug&&>(Error{})) << "Trade::GltfImporter::material(): invalid" << gltfTexture.parent()->asString() << "KHR_texture_transform texcoord property";
                    return false;
                }

                texCoord = gltfTexCoord->asUnsignedInt();
            }

            Vector2 scaling{1.0f};
            if(const Utility::JsonIterator gltfScale = gltfKhrTextureTransform->find("scale"_s)) {
                const Containers::Optional<Containers::StridedArrayView1D<const float>> scalingArray = _d->gltf->parseFloatArray(*gltfScale, 2);
                if(!scalingArray) {
                    if(!warningOnly || !(flags() & ImporterFlag::Quiet))
                        (warningOnly ? static_cast<Debug&&>(Warning{}) : static_cast<Debug&&>(Error{})) << "Trade::GltfImporter::material(): invalid" << gltfTexture.parent()->asString() << "KHR_texture_transform scale property";
                    return false;
                }

                Utility::copy(*scalingArray, scaling.data());
            }
            matrix = Matrix3::scaling(scaling)*matrix;

            Rad rotation;
            if(const Utility::JsonIterator gltfRotation = gltfKhrTextureTransform->find("rotation"_s)) {
                if(!_d->gltf->parseFloat(*gltfRotation)) {
                    if(!warningOnly || !(flags() & ImporterFlag::Quiet))
                        (warningOnly ? static_cast<Debug&&>(Warning{}) : static_cast<Debug&&>(Error{})) << "Trade::GltfImporter::material(): invalid" << gltfTexture.parent()->asString() << "KHR_texture_transform rotation property";
                    return false;
                }

                rotation = Rad(gltfRotation->asFloat());
            }
            /* Because we import images with Y flipped, counterclockwise
               rotation is now clockwise. This has to be done in addition to
               the Y flip/unflip. */
            matrix = Matrix3::rotation(-rotation)*matrix;

            Vector2 offset;
            if(const Utility::JsonIterator gltfOffset = gltfKhrTextureTransform->find("offset"_s)) {
                const Containers::Optional<Containers::StridedArrayView1D<const Float>> offsetArray = _d->gltf->parseFloatArray(*gltfOffset, 2);
                if(!offsetArray) {
                    if(!warningOnly || !(flags() & ImporterFlag::Quiet))
                        (warningOnly ? static_cast<Debug&&>(Warning{}) : static_cast<Debug&&>(Error{})) << "Trade::GltfImporter::material(): invalid" << gltfTexture.parent()->asString() << "KHR_texture_transform offset property";
                    return false;
                }

                Utility::copy(*offsetArray, offset.data());
            }
            matrix = Matrix3::translation(offset)*matrix;

            matrix = Matrix3::translation(Vector2::yAxis(1.0f))*
                     Matrix3::scaling(Vector2::yScale(-1.0f))*matrix;

            arrayAppend(attributes, InPlaceInit, matrixAttribute, matrix);
        }
    }

    /* In case the material had no texture transformation but still needs an
       Y-flip, put it there */
    if(!gltfKhrTextureTransform && _d->textureCoordinateYFlipInMaterial &&
       checkMaterialAttributeSize(matrixAttribute, MaterialAttributeType::Matrix3x3, flags()))
    {
        arrayAppend(attributes, InPlaceInit, matrixAttribute,
            Matrix3::translation(Vector2::yAxis(1.0f))*
            Matrix3::scaling(Vector2::yScale(-1.0f)));
    }

    /* Add texture coordinate set, if present. The KHR_texture_transform could
       be modifying it, so do that after */
    if(texCoord && checkMaterialAttributeSize(coordinateAttribute, MaterialAttributeType::UnsignedInt, flags()))
        arrayAppend(attributes, InPlaceInit, coordinateAttribute, *texCoord);

    /* In some cases (when dealing with packed textures), we're parsing &
       adding texture layer, coordinates and matrix multiple times, but adding
       the packed texture ID just once. In other cases the attribute is
       invalid. */
    if(!attribute.isEmpty() && checkMaterialAttributeSize(attribute, MaterialAttributeType::UnsignedInt, flags())) {
        arrayAppend(attributes, InPlaceInit, attribute, uniqueIndex);
    }

    return true;
}

Containers::Optional<MaterialData> GltfImporter::doMaterial(const UnsignedInt id) {
    const Utility::JsonToken gltfMaterial{*_d->gltf, _d->gltfMaterials[id].first()};

    Containers::Array<UnsignedInt> layers;
    Containers::Array<MaterialAttributeData> attributes;
    MaterialTypes types;

    /* Alpha mode and mask. Opaque is default in both Magnum's MaterialData and
       glTF, no need to add anything if not present. */
    if(const Utility::JsonIterator gltfAlphaMode = gltfMaterial.find("alphaMode"_s)) {
        if(!_d->gltf->parseString(*gltfAlphaMode)) {
            Error{} << "Trade::GltfImporter::material(): invalid alphaMode property";
            return {};
        }

        const Containers::StringView mode = gltfAlphaMode->asString();
        if(mode == "BLEND"_s) {
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::AlphaBlend, true);

        } else if(mode == "MASK"_s) {
            /* Cutoff is optional, defaults to 0.5 */
            Float mask = 0.5f;
            if(const Utility::JsonIterator gltfAlphaCutoff = gltfMaterial.find("alphaCutoff"_s)) {
                if(!_d->gltf->parseFloat(*gltfAlphaCutoff)) {
                    Error{} << "Trade::GltfImporter::material(): invalid alphaCutoff property";
                    return {};
                }

                mask = gltfAlphaCutoff->asFloat();
            }
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::AlphaMask, mask);

        } else if(mode == "OPAQUE"_s) {
            /* If the attribute was explicitly set to a default in the file,
               add it also explicitly here for consistency */
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::AlphaBlend, false);

        } else {
            Error{} << "Trade::GltfImporter::material(): unrecognized alphaMode" << mode;
            return {};
        }
    }

    /* Double sided. False is default in both Magnum's MaterialData and glTF,
       no need to add anything if not present. */
    if(const Utility::JsonIterator gltfDoubleSided = gltfMaterial.find("doubleSided"_s)) {
        if(!_d->gltf->parseBool(*gltfDoubleSided)) {
            Error{} << "Trade::GltfImporter::material(): invalid doubleSided property";
            return {};
        }

        arrayAppend(attributes, InPlaceInit, MaterialAttribute::DoubleSided, gltfDoubleSided->asBool());
    }

    /* Core metallic/roughness material */
    if(const Utility::JsonIterator gltfPbrMetallicRoughness = gltfMaterial.find("pbrMetallicRoughness"_s)) {
        if(!_d->gltf->parseObject(*gltfPbrMetallicRoughness)) {
            Error{} << "Trade::GltfImporter::material(): invalid pbrMetallicRoughness property";
            return {};
        }

        types |= MaterialType::PbrMetallicRoughness;

        /* Base color factor. Vector of 1.0 is default in both Magnum's
           MaterialData and glTF, no need to add anything if not present. */
        if(const Utility::JsonIterator gltfBaseColorFactor = gltfPbrMetallicRoughness->find("baseColorFactor"_s)) {
            const Containers::Optional<Containers::StridedArrayView1D<const float>> baseColorArray = _d->gltf->parseFloatArray(*gltfBaseColorFactor, 4);
            if(!baseColorArray) {
                Error{} << "Trade::GltfImporter::material(): invalid pbrMetallicRoughness baseColorFactor property";
                return {};
            }

            Color4 baseColor{NoInit};
            Utility::copy(*baseColorArray, baseColor.data());
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::BaseColor, baseColor);
        }

        /* Metallic factor. 1.0 is default in both Magnum's MaterialData and
           glTF, no need to add anything if not present. */
        if(const Utility::JsonIterator gltfMetallicFactor = gltfPbrMetallicRoughness->find("metallicFactor"_s)) {
            if(!_d->gltf->parseFloat(*gltfMetallicFactor)) {
                Error{} << "Trade::GltfImporter::material(): invalid pbrMetallicRoughness metallicFactor property";
                return {};
            }

            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::Metalness, gltfMetallicFactor->asFloat());
        }

        /* Roughness factor. 1.0 is default in both Magnum's MaterialData and
           glTF, no need to add anything if not present. */
        if(const Utility::JsonIterator gltfRoughnessFactor = gltfPbrMetallicRoughness->find("roughnessFactor"_s)) {
            if(!_d->gltf->parseFloat(*gltfRoughnessFactor)) {
                Error{} << "Trade::GltfImporter::material(): invalid pbrMetallicRoughness roughnessFactor property";
                return {};
            }

            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::Roughness, gltfRoughnessFactor->asFloat());
        }

        if(const Utility::JsonIterator gltfBaseColorTexture = gltfPbrMetallicRoughness->find("baseColorTexture"_s)) {
            if(!materialTexture(*gltfBaseColorTexture, attributes, "BaseColorTexture"_s))
                return {};
        }

        if(const Utility::JsonIterator gltfMetallicRoughnessTexture = gltfPbrMetallicRoughness->find("metallicRoughnessTexture"_s)) {
            if(!materialTexture(*gltfMetallicRoughnessTexture, attributes,
                "NoneRoughnessMetallicTexture"_s,
                "MetalnessTexture"_s)
            )
                return {};

            /* Add the matrix/coordinates attributes also for the roughness
               texture, but skip adding the texture ID again. If the above
               didn't fail, this shouldn't either. */
            CORRADE_INTERNAL_ASSERT_OUTPUT(materialTexture(
                *gltfMetallicRoughnessTexture, attributes,
                {},
                "RoughnessTexture"_s));
        }

        /** @todo Support for KHR_materials_specular? This adds an explicit
            F0 (texture) and a scalar factor (texture) for the entire specular
            reflection to a metallic/roughness material. Currently imported as
            a custom layer below. */
    }

    /* Extensions. Go through the object, filter out what's supported directly
       and put the rest into a list to be processed later. */
    Utility::JsonIterator gltfPbrSpecularGlossiness;
    Utility::JsonIterator gltfUnlit;
    Utility::JsonIterator gltfClearcoat;
    /* Saving the key name separately to not need to repeatedly call
       token.asString() during sort below. Similarly not storing
       Utility::JsonTokenData& as this is just a temporary container so twice
       the size doesn't matter and creating a JsonToken on-the-fly in the hot
       std::sort() loop doesn't make sense perf-wise either. */
    Containers::Array<Containers::Pair<Containers::StringView, Utility::JsonToken>> gltfExtensions;
    if(const Utility::JsonIterator gltfExtensionList = gltfMaterial.find("extensions"_s)) {
        if(!_d->gltf->parseObject(*gltfExtensionList)) {
            Error{} << "Trade::GltfImporter::material(): invalid extensions property";
            return {};
        }

        for(Utility::JsonObjectItem gltfExtension: gltfExtensionList->asObject()) {
            const Containers::StringView extensionName = gltfExtension.key();
            if(!_d->gltf->parseObject(gltfExtension.value())) {
                Error{} << "Trade::GltfImporter::material(): invalid" << extensionName << "extension property";
                return {};
            }

            if(extensionName == "KHR_materials_pbrSpecularGlossiness"_s)
                gltfPbrSpecularGlossiness = gltfExtension.value();
            else if(extensionName == "KHR_materials_unlit"_s)
                gltfUnlit = gltfExtension.value();
            else if(extensionName == "KHR_materials_clearcoat"_s)
                gltfClearcoat = gltfExtension.value();
            else
                arrayAppend(gltfExtensions, InPlaceInit, extensionName, gltfExtension.value());
        }
    }

    /* Specular/glossiness material */
    if(gltfPbrSpecularGlossiness) {
        types |= MaterialType::PbrSpecularGlossiness;

        /* Token checked for object type above already */

        /* Diffuse factor. Vector of 1.0 is default in both Magnum's
           MaterialData and glTF, no need to add anything if not present. */
        if(const Utility::JsonIterator gltfDiffuseFactor = gltfPbrSpecularGlossiness->find("diffuseFactor"_s)) {
            const Containers::Optional<Containers::StridedArrayView1D<const float>> diffuseFactorArray = _d->gltf->parseFloatArray(*gltfDiffuseFactor, 4);
            if(!diffuseFactorArray) {
                Error{} << "Trade::GltfImporter::material(): invalid KHR_materials_pbrSpecularGlossiness diffuseFactor property";
                return {};
            }

            Color4 diffuseFactor{NoInit};
            Utility::copy(*diffuseFactorArray, diffuseFactor.data());
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::DiffuseColor, diffuseFactor);
        }

        /* Specular factor. Vector of 1.0 is default in both Magnum's
           MaterialData and glTF, no need to add anything if not present. */
        if(const Utility::JsonIterator gltfSpecularFactor = gltfPbrSpecularGlossiness->find("specularFactor"_s)) {
            const Containers::Optional<Containers::StridedArrayView1D<const float>> specularFactorArray = _d->gltf->parseFloatArray(*gltfSpecularFactor, 3);
            if(!specularFactorArray) {
                Error{} << "Trade::GltfImporter::material(): invalid KHR_materials_pbrSpecularGlossiness specularFactor property";
                return {};
            }

            /* Specular is 3-component in glTF, alpha should be 0 to not
               affect transparent materials */
            Color4 specularFactor{NoInit};
            specularFactor.a() = 0.0f;
            Utility::copy(*specularFactorArray, Containers::arrayView(specularFactor.data()).prefix(3));
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::SpecularColor, specularFactor);
        }

        /* Glossiness factor. 1.0 is default in both Magnum's MaterialData and
           glTF, no need to add anything if not present. */
        if(const Utility::JsonIterator gltfGlossinessFactor = gltfPbrSpecularGlossiness->find("glossinessFactor"_s)) {
            if(!_d->gltf->parseFloat(*gltfGlossinessFactor)) {
                Error{} << "Trade::GltfImporter::material(): invalid KHR_materials_pbrSpecularGlossiness glossinessFactor property";
                return {};
            }

            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::Glossiness, gltfGlossinessFactor->asFloat());
        }

        if(const Utility::JsonIterator gltfBaseColorTexture = gltfPbrSpecularGlossiness->find("diffuseTexture"_s)) {
            if(!materialTexture(*gltfBaseColorTexture, attributes, "DiffuseTexture"_s))
                return {};
        }

        if(const Utility::JsonIterator gltfSpecularGlossinessTexture = gltfPbrSpecularGlossiness->find("specularGlossinessTexture"_s)) {
            if(!materialTexture(*gltfSpecularGlossinessTexture, attributes,
                "SpecularGlossinessTexture"_s,
                "SpecularTexture"_s)
            )
                return {};

            /* Add the matrix/coordinates attributes also for the glossiness
               texture, but skip adding the texture ID again. If the above
               didn't fail, this shouldn't either. */
            CORRADE_INTERNAL_ASSERT_OUTPUT(materialTexture(
                *gltfSpecularGlossinessTexture, attributes,
                {},
                "GlossinessTexture"_s));
        }
    }

    /* Unlit material -- reset all types and add just Flat */
    if(gltfUnlit) {
        types = MaterialType::Flat;

        /* Token checked for object type above already */
    }

    /* Normal texture */
    if(const Utility::JsonIterator gltfNormalTexture = gltfMaterial.find("normalTexture"_s)) {
        if(!materialTexture(*gltfNormalTexture, attributes, "NormalTexture"_s))
            return {};

        /* Scale. 1.0 is default in both Magnum's MaterialData and glTF, no
           need to add anything if not present. */
        if(const Utility::JsonIterator gltfNormalTextureScale = gltfNormalTexture->find("scale"_s)) {
            if(!_d->gltf->parseFloat(*gltfNormalTextureScale)) {
                Error{} << "Trade::GltfImporter::material(): invalid normalTexture scale property";
                return {};
            }

            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::NormalTextureScale,
                gltfNormalTextureScale->asFloat());
        }
    }

    /* Occlusion texture */
    if(const Utility::JsonIterator gltfOcclusionTexture = gltfMaterial.find("occlusionTexture"_s)) {
        if(!materialTexture(*gltfOcclusionTexture, attributes, "OcclusionTexture"_s))
            return {};

        /* Strength. 1.0 is default in both Magnum's MaterialData and glTF, no
           need to add anything if not present. */
        if(const Utility::JsonIterator gltfOcclusionTextureStrength = gltfOcclusionTexture->find("strength"_s)) {
            if(!_d->gltf->parseFloat(*gltfOcclusionTextureStrength)) {
                Error{} << "Trade::GltfImporter::material(): invalid occlusionTexture strength property";
                return {};
            }

            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::OcclusionTextureStrength,
                gltfOcclusionTextureStrength->asFloat());
        }
    }

    /* Emissive factor. Vector of 1.0 is default in both Magnum's MaterialData
       and glTF, no need to add anything if not present. */
    if(const Utility::JsonIterator gltfEmissiveFactor = gltfMaterial.find("emissiveFactor"_s)) {
        const Containers::Optional<Containers::StridedArrayView1D<const float>> emissiveFactorArray = _d->gltf->parseFloatArray(*gltfEmissiveFactor, 3);
        if(!emissiveFactorArray) {
            Error{} << "Trade::GltfImporter::material(): invalid emissiveFactor property";
            return {};
        }

        Color3 emissiveColor{NoInit};
        Utility::copy(*emissiveFactorArray, emissiveColor.data());
        arrayAppend(attributes, InPlaceInit,
            MaterialAttribute::EmissiveColor, emissiveColor);
    }

    /* Emissive texture */
    if(const Utility::JsonIterator gltfEmissiveTexture = gltfMaterial.find("emissiveTexture"_s)) {
        if(!materialTexture(*gltfEmissiveTexture, attributes, "EmissiveTexture"_s))
            return {};
    }

    /* Phong material fallback for backwards compatibility */
    if(configuration().value<bool>("phongMaterialFallback")) {
        /* This adds a Phong type even to Flat materials because that's exactly
           how it behaved before */
        types |= MaterialType::Phong;

        /* Create Diffuse attributes from BaseColor */
        Containers::Optional<Color4> diffuseColor;
        Containers::Optional<UnsignedInt> diffuseTexture;
        Containers::Optional<Matrix3> diffuseTextureMatrix;
        Containers::Optional<UnsignedInt> diffuseTextureCoordinates;
        Containers::Optional<UnsignedInt> diffuseTextureLayer;
        for(const MaterialAttributeData& attribute: attributes) {
            if(attribute.name() == "BaseColor"_s)
                diffuseColor = attribute.value<Color4>();
            else if(attribute.name() == "BaseColorTexture"_s)
                diffuseTexture = attribute.value<UnsignedInt>();
            else if(attribute.name() == "BaseColorTextureMatrix"_s)
                diffuseTextureMatrix = attribute.value<Matrix3>();
            else if(attribute.name() == "BaseColorTextureCoordinates"_s)
                diffuseTextureCoordinates = attribute.value<UnsignedInt>();
            else if(attribute.name() == "BaseColorTextureLayer"_s)
                diffuseTextureLayer = attribute.value<UnsignedInt>();
        }

        /* But if there already are those from the specular/glossiness
           material, don't add them again. Has to be done in a separate pass
           to avoid resetting too early. */
        for(const MaterialAttributeData& attribute: attributes) {
            if(attribute.name() == "DiffuseColor"_s)
                diffuseColor = Containers::NullOpt;
            else if(attribute.name() == "DiffuseTexture"_s)
                diffuseTexture = Containers::NullOpt;
            else if(attribute.name() == "DiffuseTextureMatrix"_s)
                diffuseTextureMatrix = Containers::NullOpt;
            else if(attribute.name() == "DiffuseTextureCoordinates"_s)
                diffuseTextureCoordinates = Containers::NullOpt;
            else if(attribute.name() == "DiffuseTextureLayer"_s)
                diffuseTextureLayer = Containers::NullOpt;
        }

        if(diffuseColor)
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::DiffuseColor, *diffuseColor);
        if(diffuseTexture)
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::DiffuseTexture, *diffuseTexture);
        if(diffuseTextureMatrix)
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::DiffuseTextureMatrix, *diffuseTextureMatrix);
        if(diffuseTextureCoordinates)
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::DiffuseTextureCoordinates, *diffuseTextureCoordinates);
        if(diffuseTextureLayer)
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::DiffuseTextureLayer, *diffuseTextureLayer);
    }

    /* Used by three stableSortRemoveDuplicatesToPrefix() calls below for
       extras / extensions */
    const auto extraOrExtensionKeyCompare = [](const Containers::Pair<Containers::StringView, Utility::JsonToken>& a, const Containers::Pair<Containers::StringView, Utility::JsonToken>& b) {
        /* If the names are different, sort by those */
        if(a.first() != b.first())
            return a.first() < b.first();
        /* Otherwise, sort the earlier occurence first. Cannot use
           {a,b}.first().data() since the key can contain an escape character,
           which would then cause the string to be allocated elsewhere instead
           of pointing to the JSON input, making the order random again. The
           token data string view data pointer however *is* pointing to the
           JSON input always. The token address itself could also since they're
           stored in a contiguous array, but this is more robust. */
        return a.second().data().data() < b.second().data().data();
    };
    const auto extraOrExtensionKeyEqual = [](const Containers::Pair<Containers::StringView, Utility::JsonToken>& a, const Containers::Pair<Containers::StringView, Utility::JsonToken>& b) {
        return a.first() == b.first();
    };

    /* Extras -- application-specific data, added to the base layer */
    if(const Utility::JsonIterator gltfExtras = gltfMaterial.find("extras"_s)) {
        /* Theoretically extras can be any token type but the glTF spec
           recommends objects for interoperability, makes our life easier, too:
           https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#reference-extras */
        if(gltfExtras->type() == Utility::JsonToken::Type::Object) {
            bool parsed;
            {
                /* Redirect error messages from Json::parse*() to the warning
                   output as they are non-fatal and only lead to the extras
                   being skipped. If quiet output is requested, don't print
                   them at all. */
                Error redirectError{flags() & ImporterFlag::Quiet ? nullptr : Warning::output()};
                parsed = !!_d->gltf->parseObject(*gltfExtras);
            }
            if(parsed) {
                /* Saving the key name separately to not need to repeatedly
                   call token.asString() during sort below */
                Containers::Array<Containers::Pair<Containers::StringView, Utility::JsonToken>> gltfExtraItems;
                for(const Utility::JsonObjectItem i: gltfExtras->asObject())
                    arrayAppend(gltfExtraItems, InPlaceInit, i.key(), i.value());

                /* Sort and remove duplicates except the last one. We don't
                   need to cross-check for duplicates in the base layer because
                   those are all internal uppercase names and we make all names
                   lowercase. */
                const std::size_t uniqueCount = stableSortRemoveDuplicatesToPrefix(arrayView(gltfExtraItems), extraOrExtensionKeyCompare, extraOrExtensionKeyEqual);

                arrayReserve(attributes, attributes.size() + uniqueCount);
                /** @todo use suffix() once it takes suffix size and not prefix size */
                for(const Containers::Pair<Containers::StringView, Utility::JsonToken>& gltfExtraItem: gltfExtraItems.exceptPrefix(gltfExtraItems.size() - uniqueCount)) {
                    if(const Containers::Optional<MaterialAttributeData> parsedAttribute = parseMaterialAttribute(*_d->gltf, gltfExtraItem.first(), gltfExtraItem.second(), flags()))
                        arrayAppend(attributes, *parsedAttribute);
                }

            } else if(!(flags() & ImporterFlag::Quiet))
                Warning{} << "Trade::GltfImporter::material(): extras object has invalid keys, skipping";

        } else if(!(flags() & ImporterFlag::Quiet))
            Warning{} << "Trade::GltfImporter::material(): extras property is not an object, skipping";
    }

    /* Clear coat layer -- needs to be after all base material attributes */
    if(gltfClearcoat) {
        types |= MaterialType::PbrClearCoat;

        /* Token checked for object type above already */

        /* Add a new layer -- this works both if layers are empty and if
           there's something already */
        arrayAppend(layers, UnsignedInt(attributes.size()));
        arrayAppend(attributes, InPlaceInit, MaterialLayer::ClearCoat);

        /* Weirdly enough, the KHR_materials_clearcoat has the factor
           defaulting to 0.0, which means if a texture is specified both have
           to be present to not have the texture canceled out. Magnum has 1.0
           as a default, so we add an explicit 0.0 if the factor is not
           present. */
        if(const Utility::JsonIterator gltfClearcoatFactor = gltfClearcoat->find("clearcoatFactor"_s)) {
            if(!_d->gltf->parseFloat(*gltfClearcoatFactor)) {
                Error{} << "Trade::GltfImporter::material(): invalid KHR_materials_clearcoat clearcoatFactor property";
                return {};
            }

            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::LayerFactor, gltfClearcoatFactor->asFloat());
        } else arrayAppend(attributes, InPlaceInit,
            MaterialAttribute::LayerFactor, 0.0f);

        if(const Utility::JsonIterator gltfClearcoatTexture = gltfClearcoat->find("clearcoatTexture"_s)) {
            if(!materialTexture(*gltfClearcoatTexture, attributes, "LayerFactorTexture"_s))
                return {};
        }

        /* Same as with gltfClearcoatFactor -- Magnum's default is 1.0 to not
           have to specify both if a texture is present, so add an explicit 0.0
           if the factor is not present. */
        if(const Utility::JsonIterator gltfRoughnessFactor = gltfClearcoat->find("clearcoatRoughnessFactor"_s)) {
            if(!_d->gltf->parseFloat(*gltfRoughnessFactor)) {
                Error{} << "Trade::GltfImporter::material(): invalid KHR_materials_clearcoat roughnessFactor property";
                return {};
            }

            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::Roughness, gltfRoughnessFactor->asFloat());
        } else arrayAppend(attributes, InPlaceInit,
            MaterialAttribute::Roughness, 0.0f);

        if(const Utility::JsonIterator gltfRoughnessTexture = gltfClearcoat->find("clearcoatRoughnessTexture"_s)) {
            if(!materialTexture(*gltfRoughnessTexture, attributes, "RoughnessTexture"_s))
                return {};

            /* The extension description doesn't mention it, but the schema
               says the clearcoat roughness is actually in the G channel:
               https://github.com/KhronosGroup/glTF/blob/dc5519b9ce9834f07c30ec4c957234a0cd6280a2/extensions/2.0/Khronos/KHR_materials_clearcoat/schema/glTF.KHR_materials_clearcoat.schema.json#L32 */
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::RoughnessTextureSwizzle,
                MaterialTextureSwizzle::G);
        }

        if(const Utility::JsonIterator gltfNormalTexture = gltfClearcoat->find("clearcoatNormalTexture"_s)) {
            if(!materialTexture(*gltfNormalTexture, attributes, "NormalTexture"_s))
                return {};

            /* Scale. 1.0 is default in both Magnum's MaterialData and glTF, no
               need to add anything if not present. */
            if(const Utility::JsonIterator gltfNormalTextureScale = gltfNormalTexture->find("scale"_s)) {
                if(!_d->gltf->parseFloat(*gltfNormalTextureScale)) {
                    Error{} << "Trade::GltfImporter::material(): invalid KHR_materials_clearcoat normalTexture scale property";
                    return {};
                }

                arrayAppend(attributes, InPlaceInit,
                    MaterialAttribute::NormalTextureScale,
                    gltfNormalTextureScale->asFloat());
            }
        }
    }

    /* Sort and remove duplicates in remaining extensions */
    const std::size_t uniqueExtensionCount = stableSortRemoveDuplicatesToPrefix(arrayView(gltfExtensions), extraOrExtensionKeyCompare, extraOrExtensionKeyEqual);

    /* Import unrecognized extension attributes as custom attributes, one
       layer per extension */
    /** @todo use suffix() once it takes suffix size and not prefix size */
    for(const Containers::Pair<Containers::StringView, Utility::JsonToken>& gltfExtension: gltfExtensions.exceptPrefix(gltfExtensions.size() - uniqueExtensionCount)) {
        if(!gltfExtension.first()) {
            if(!(flags() & ImporterFlag::Quiet))
                Warning{} << "Trade::GltfImporter::material(): extension with an empty name, skipping";
            continue;
        }

        /* +1 is the key null byte. +3 are the '#' layer prefix, the layer null
           byte and the length. */
        if(" LayerName"_s.size() + 1 + gltfExtension.first().size() + 3 + sizeof(MaterialAttributeType) > sizeof(MaterialAttributeData)) {
            if(!(flags() & ImporterFlag::Quiet))
                Warning{} << "Trade::GltfImporter::material(): extension name" << gltfExtension.first() << "is too long with" << gltfExtension.first().size() << "characters, skipping";
            continue;
        }

        /* Saving the key name separately to not need to repeatedly call
           token.asString() during sort below */
        Containers::Array<Containers::Pair<Containers::StringView, Utility::JsonToken>> gltfExtensionItems;
        for(const Utility::JsonObjectItem i: gltfExtension.second().asObject())
            arrayAppend(gltfExtensionItems, InPlaceInit, i.key(), i.value());

        /* Sort and remove duplicates */
        const std::size_t uniqueCount = stableSortRemoveDuplicatesToPrefix(arrayView(gltfExtensionItems), extraOrExtensionKeyCompare, extraOrExtensionKeyEqual);

        arrayAppend(layers, attributes.size());
        arrayReserve(attributes, attributes.size() + uniqueCount + 1);
        /* Uppercase layer names are reserved. Since all extension names start
           with an uppercase vendor identifier, making the first character
           lowercase seems silly, so we use a unique prefix. */
        arrayAppend(attributes, InPlaceInit, MaterialAttribute::LayerName, Utility::format("#{}", gltfExtension.first()));
        /** @todo use suffix() once it takes suffix size and not prefix size */
        for(const Containers::Pair<Containers::StringView, Utility::JsonToken>& gltfExtensionItem: gltfExtensionItems.exceptPrefix(gltfExtensionItems.size() - uniqueCount)) {
            if(!gltfExtensionItem.first()) {
                if(!(flags() & ImporterFlag::Quiet))
                    Warning{} << "Trade::GltfImporter::material(): property with an empty name, skipping";
                continue;
            }

            /* Parse glTF textureInfo objects. Any objects without the correct
               suffix and type are ignored. */
            if(gltfExtensionItem.second().type() == Utility::JsonToken::Type::Object) {
                if(gltfExtensionItem.first().size() < 8 || !gltfExtensionItem.first().hasSuffix("Texture"_s)) {
                    if(!(flags() & ImporterFlag::Quiet))
                        Warning{} << "Trade::GltfImporter::material(): property" << gltfExtensionItem.first() << "has a non-texture object type, skipping";
                    continue;
                }

                if(!materialTexture(gltfExtensionItem.second(), attributes, gltfExtensionItem.first(), /* warningOnly */ true)) {
                    if(!(flags() & ImporterFlag::Quiet))
                        Warning{} << "Trade::GltfImporter::material(): property" << gltfExtensionItem.first() << "has an invalid texture object, skipping";
                    continue;
                }

                /** @todo If there are ever extensions that reference texture
                    types other than textureInfo and normalTextureInfo, this
                    should instead loop through the texture properties, filter
                    out what's handled by materialTexture() and add the rest,
                    basically same as done for extras */
                if(const Utility::JsonIterator gltfTextureScale = gltfExtensionItem.second().find("scale"_s)) {
                    bool parsed;
                    {
                        /* Redirect error messages from Json::parse*() to the
                           warning output as they are non-fatal and only lead
                           to given attribute being skipped. If quiet output
                           is requested, don't print them at all. */
                        Error redirectError{flags() & ImporterFlag::Quiet ? nullptr : Warning::output()};
                        parsed = !!_d->gltf->parseFloat(*gltfTextureScale);
                    }
                    if(!parsed) {
                        if(!(flags() & ImporterFlag::Quiet))
                            Warning{} << "Trade::GltfImporter::material(): invalid" << gltfExtension.first() << gltfExtensionItem.first() << "scale property, skipping";
                        continue;
                    }

                    /** @todo avoid allocations when it's doable in a less
                        error-prone way than formatInto() + slice() (ArrayTuple
                        string support?), best with a statically-sized buffer */
                    const Containers::String scaleName = gltfExtensionItem.first() + "Scale"_s;
                    if(checkMaterialAttributeSize(scaleName, MaterialAttributeType::Float, flags()))
                        arrayAppend(attributes, InPlaceInit,
                            scaleName, gltfTextureScale->asFloat());
                }

            } else {
                /* All other attribute types: bool, numbers, strings */
                if(const Containers::Optional<MaterialAttributeData> parsed = parseMaterialAttribute(*_d->gltf, gltfExtensionItem.first(), gltfExtensionItem.second(), flags()))
                    arrayAppend(attributes, *parsed);
            }
        }
    }

    /* If there's any layer, add the final attribute count */
    arrayAppend(layers, UnsignedInt(attributes.size()));

    /* Can't use growable deleters in a plugin, convert back to the default
       deleter */
    arrayShrink(layers);
    arrayShrink(attributes, DefaultInit);
    return MaterialData{types, Utility::move(attributes), Utility::move(layers), &gltfMaterial.token()};
}

UnsignedInt GltfImporter::doTextureCount() const {
    return _d->uniqueTextures.size();
}

Int GltfImporter::doTextureForName(const Containers::StringView name) {
    if(!_d->texturesForName) {
        _d->texturesForName.emplace();
        _d->texturesForName->reserve(_d->uniqueTextures.size());
        for(std::size_t i = 0; i != _d->uniqueTextures.size(); ++i)
            if(const Containers::StringView n = _d->gltfTextures[_d->uniqueTextures[i]].second())
                _d->texturesForName->emplace(n, i);
    }

    const auto found = _d->texturesForName->find(name);
    return found == _d->texturesForName->end() ? -1 : found->second;
}

Containers::String GltfImporter::doTextureName(const UnsignedInt id) {
    return _d->gltfTextures[_d->uniqueTextures[id]].second();
}

Containers::Optional<TextureData> GltfImporter::doTexture(const UnsignedInt id) {
    const Utility::JsonToken gltfTexture{*_d->gltf, _d->gltfTextures[_d->uniqueTextures[id]].first()};

    Utility::JsonIterator gltfSource;

    /* Various extensions, they override the standard image. The core glTF spec
       only allows image/jpeg and image/png and these extend for other MIME
       types. We don't really care as we delegate to AnyImageImporter and let
       it figure out the file type based on magic, so we just pick the first
       available image, assuming that extension order indicates a preference
       ... */
    /** @todo Figure out a better priority
        - extensionsRequired?
        - image importers available via manager()->aliasList()?
        - are there even files out there with more than one extension? */
    if(const Utility::JsonIterator gltfExtensions = gltfTexture.find("extensions"_s)) {
        if(!_d->gltf->parseObject(*gltfExtensions)) {
            Error{} << "Trade::GltfImporter::texture(): invalid extensions property";
            return {};
        }

        /* Pick the first extension we understand */
        for(const Utility::JsonObjectItem i: gltfExtensions->asObject()) {
            const Containers::StringView extensionName = i.key();

            if(!(extensionName == "KHR_texture_ktx"_s && configuration().value<bool>("experimentalKhrTextureKtx")) &&
               !isRecognized2DTextureExtension(extensionName)
            )
                continue;

            if(!_d->gltf->parseObject(i.value())) {
                Error{} << "Trade::GltfImporter::texture(): invalid" << extensionName << "extension";
                return {};
            }

            /* Retrieve the source here already and not in common code below so
               we can include the extension name in the error message. For the
               image index bounds check it's not as important as the offending
               extension can be located from the reported image ID. */
            gltfSource = i.value().find("source"_s);
            if(!gltfSource || !_d->gltf->parseUnsignedInt(*gltfSource)) {
                Error{} << "Trade::GltfImporter::texture(): missing or invalid" << extensionName << "source property";
                return {};
            }

            break;
        }
    }

    /* ... and the core image is a fallback if everything else fails */
    if(!gltfSource) {
        gltfSource = gltfTexture.find("source"_s);
        if(!gltfSource || !_d->gltf->parseUnsignedInt(*gltfSource)) {
            Error{} << "Trade::GltfImporter::texture(): missing or invalid source property";
            return {};
        }
    }

    if(gltfSource->asUnsignedInt() >= _d->gltfImages.size()) {
        Error{} << "Trade::GltfImporter::texture(): index" << gltfSource->asUnsignedInt() << "out of range for" << _d->gltfImages.size() << "images";
        return {};
    }

    /* Texture is 2D if the referenced image is 2D, or an array if the
       referenced image is a 2D array */
    UnsignedInt image = _d->imageByDimensionForGltfImage[gltfSource->asUnsignedInt()];
    TextureType type = TextureType::Texture2D;
    if(image >= _d->image2DCount) {
        image -= _d->image2DCount;
        type = TextureType::Texture2DArray;
    }

    /* Sampler. If it's not referenced, the specification instructs to use
       "auto filtering and repeat wrapping", i.e. it is left to the implementor
       to decide on the default values... */
    SamplerFilter minificationFilter = SamplerFilter::Linear;
    SamplerFilter magnificationFilter = SamplerFilter::Linear;
    SamplerMipmap mipmap = SamplerMipmap::Linear;
    Math::Vector3<SamplerWrapping> wrapping{SamplerWrapping::Repeat};
    if(const Utility::JsonIterator gltfSamplerIndex = gltfTexture.find("sampler"_s)) {
        if(!_d->gltf->parseUnsignedInt(*gltfSamplerIndex)) {
            Error{} << "Trade::GltfImporter::texture(): invalid sampler property";
            return {};
        }
        if(gltfSamplerIndex->asUnsignedInt() >= _d->gltfSamplers.size()) {
            Error{} << "Trade::GltfImporter::texture(): index" << gltfSamplerIndex->asUnsignedInt() << "out of range for" << _d->gltfSamplers.size() << "samplers";
            return {};
        }

        Containers::Optional<Sampler>& storage = _d->samplers[gltfSamplerIndex->asUnsignedInt()];
        if(storage) {
            minificationFilter = storage->minificationFilter;
            magnificationFilter = storage->magnificationFilter;
            mipmap = storage->mipmap;
            wrapping = storage->wrapping;
        } else {
            const Utility::JsonToken gltfSampler{*_d->gltf, _d->gltfSamplers[gltfSamplerIndex->asUnsignedInt()]};

            /* Magnification filter */
            if(const Utility::JsonIterator gltfMagFilter = gltfSampler.find("magFilter"_s)) {
                if(!_d->gltf->parseUnsignedInt(*gltfMagFilter)) {
                    Error{} << "Trade::GltfImporter::texture(): invalid magFilter property";
                    return {};
                }
                switch(gltfMagFilter->asUnsignedInt()) {
                    case Implementation::GltfFilterNearest:
                        magnificationFilter = SamplerFilter::Nearest;
                        break;
                    case Implementation::GltfFilterLinear:
                        magnificationFilter = SamplerFilter::Linear;
                        break;
                    default:
                        Error{} << "Trade::GltfImporter::texture(): unrecognized magFilter" << gltfMagFilter->asUnsignedInt();
                        return {};
                }
            }

            /* Minification filter */
            if(const Utility::JsonIterator gltfMinFilter = gltfSampler.find("minFilter"_s)) {
                if(!_d->gltf->parseUnsignedInt(*gltfMinFilter)) {
                    Error{} << "Trade::GltfImporter::texture(): invalid minFilter property";
                    return {};
                }
                switch(gltfMinFilter->asUnsignedInt()) {
                    /* LCOV_EXCL_START */
                    case Implementation::GltfFilterNearest:
                        minificationFilter = SamplerFilter::Nearest;
                        mipmap = SamplerMipmap::Base;
                        break;
                    case Implementation::GltfFilterNearestMipmapNearest:
                        minificationFilter = SamplerFilter::Nearest;
                        mipmap = SamplerMipmap::Nearest;
                        break;
                    case Implementation::GltfFilterNearestMipmapLinear:
                        minificationFilter = SamplerFilter::Nearest;
                        mipmap = SamplerMipmap::Linear;
                        break;
                    case Implementation::GltfFilterLinear:
                        minificationFilter = SamplerFilter::Linear;
                        mipmap = SamplerMipmap::Base;
                        break;
                    case Implementation::GltfFilterLinearMipmapNearest:
                        minificationFilter = SamplerFilter::Linear;
                        mipmap = SamplerMipmap::Nearest;
                        break;
                    case Implementation::GltfFilterLinearMipmapLinear:
                        minificationFilter = SamplerFilter::Linear;
                        mipmap = SamplerMipmap::Linear;
                        break;
                    /* LCOV_EXCL_STOP */
                    default:
                        Error{} << "Trade::GltfImporter::texture(): unrecognized minFilter" << gltfMinFilter->asUnsignedInt();
                        return {};
                }
            }

            /* Wrapping */
            for(const char coordinate: {0, 1}) {
                /* No, I'm definitely not overdoing anything here */
                const char name[]{'w', 'r', 'a', 'p', char('S' + coordinate)};
                if(const Utility::JsonIterator gltfWrap = gltfSampler.find({name, sizeof(name)})) {
                    if(!_d->gltf->parseUnsignedInt(*gltfWrap)) {
                        Error{} << "Trade::GltfImporter::texture(): invalid" << Containers::StringView{name, sizeof(name)} << "property";
                        return {};
                    }
                    switch(gltfWrap->asUnsignedInt()) {
                        /* LCOV_EXCL_START */
                        case Implementation::GltfWrappingClampToEdge:
                            wrapping[coordinate] = SamplerWrapping::ClampToEdge;
                            break;
                        case Implementation::GltfWrappingMirroredRepeat:
                            wrapping[coordinate] = SamplerWrapping::MirroredRepeat;
                            break;
                        case Implementation::GltfWrappingRepeat:
                            wrapping[coordinate] = SamplerWrapping::Repeat;
                            break;
                        /* LCOV_EXCL_STOP */
                        default:
                            Error{} << "Trade::GltfImporter::texture(): unrecognized" << Containers::StringView{name, sizeof(name)} << gltfWrap->asUnsignedInt();
                            return {};
                    }
                }
            }

            storage.emplace(
                minificationFilter,
                magnificationFilter,
                mipmap,
                wrapping);
        }
    }

    return TextureData{type, minificationFilter, magnificationFilter,
        /* In case of KHR_texture_ktx deduplication, this returns the first
           texture in the chain */
        /** @todo when we have arbirary key/value storage, store all there? */
        mipmap, wrapping, image, &gltfTexture.token()};
}

AbstractImporter* GltfImporter::setupOrReuseImporterForImage(const char* const errorPrefix, const UnsignedInt id, const UnsignedInt expectedDimensions) {
    /* Looking for the same ID, so reuse an importer populated before. If the
       previous attempt failed, the importer is not set, so return nullptr in
       that case. Going through everything below again would not change the
       outcome anyway, only spam the output with redundant messages. */
    if(_d->imageImporterId == id)
        return _d->imageImporter ? &*_d->imageImporter : nullptr;

    /* Otherwise reset the importer and remember the new ID. If the import
       fails, the importer will stay unset, but the ID will be updated so the
       next round can again just return nullptr above instead of going through
       the doomed-to-fail process again. */
    _d->imageImporter = Containers::NullOpt;
    _d->imageImporterId = id;

    AnyImageImporter importer{*manager()};
    importer.setFlags(flags());
    if(fileCallback())
        importer.setFileCallback(fileCallback(), fileCallbackUserData());

    const Utility::JsonToken gltfImage{*_d->gltf, _d->gltfImages[id].first()};

    const Utility::JsonIterator gltfUri = gltfImage.find("uri"_s);
    if(gltfUri && !_d->gltf->parseString(*gltfUri)) {
        Error{} << errorPrefix << "invalid uri property";
        return {};
    }

    const Utility::JsonIterator gltfBufferView = gltfImage.find("bufferView"_s);
    if(gltfBufferView && !_d->gltf->parseUnsignedInt(*gltfBufferView)) {
        Error{} << errorPrefix << "invalid bufferView property";
        return {};
    }

    /* Should have either an uri or a buffer view and not both */
    if(!!gltfUri == !!gltfBufferView) {
        Error{} << errorPrefix << "expected exactly one of uri or bufferView properties defined";
        return {};
    }

    /* Load embedded image. Can either be a buffer view or a base64 payload.
       Buffers are kept in memory until the importer closes but decoded base64
       data is freed after opening the image. */
    if(!gltfUri || isDataUri(gltfUri->asString())) {
        Containers::Optional<Containers::Array<char>> imageData;
        Containers::ArrayView<const void> imageView;

        if(gltfUri) {
            if(!(imageData = loadUri(errorPrefix, gltfUri->asString())))
                return {};
            imageView = *imageData;

        } else if(gltfBufferView) {
            const Containers::Optional<BufferView> bufferView = parseBufferView(errorPrefix, gltfBufferView->asUnsignedInt());
            if(!bufferView)
                return {};

            /* 3.6.1.1. (Binary Data Storage § Buffers and Buffer Views §
               Overview) says "Buffer views with [non-vertex] types of data
               MUST NOT not define byteStride", which makes sense */
            if(bufferView->stride) {
                Error{} << errorPrefix << "buffer view" << gltfBufferView->asUnsignedInt() << "is strided";
                return {};
            }

            imageView = bufferView->data;

        } else CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */

        if(!importer.openData(imageView))
            return nullptr;
        return &_d->imageImporter.emplace(Utility::move(importer));
    }

    /* Load external image */
    if(!_d->filename && !fileCallback()) {
        Error{} << errorPrefix << "external images can be imported only when opening files from the filesystem or if a file callback is present";
        return nullptr;
    }

    const Containers::Optional<Containers::String> decodedUri = decodeUri(errorPrefix, gltfUri->asString());
    if(!decodedUri)
        return nullptr;
    if(!importer.openFile(Utility::Path::join(_d->filename ? Utility::Path::path(*_d->filename) : Containers::StringView{}, *decodedUri)))
        return nullptr;

    UnsignedInt expectedDimensionsImageCount;
    const char* expectedDimensionsString;
    if(expectedDimensions == 2) {
        expectedDimensionsImageCount = importer.image2DCount();
        expectedDimensionsString = "2D";
    } else if(expectedDimensions == 3) {
        expectedDimensionsImageCount = importer.image3DCount();
        expectedDimensionsString = "3D";
    } else CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
    if(expectedDimensionsImageCount != 1) {
        Error{} << errorPrefix << "expected exactly one" << expectedDimensionsString << "image in an image file but got" << expectedDimensionsImageCount;
        return nullptr;
    }

    return &_d->imageImporter.emplace(Utility::move(importer));
}

UnsignedInt GltfImporter::doImage2DCount() const {
    return _d->image2DCount;
}

Int GltfImporter::doImage2DForName(const Containers::StringView name) {
    if(!_d->images2DForName) {
        _d->images2DForName.emplace();
        _d->images2DForName->reserve(_d->image2DCount);
        for(std::size_t i = 0; i != _d->image2DCount; ++i)
            if(const Containers::StringView n = _d->gltfImages[_d->imagesByDimension[i]].second())
                _d->images2DForName->emplace(n, i);
    }

    const auto found = _d->images2DForName->find(name);
    return found == _d->images2DForName->end() ? -1 : found->second;
}

Containers::String GltfImporter::doImage2DName(const UnsignedInt id) {
    return _d->gltfImages[_d->imagesByDimension[id]].second();
}

UnsignedInt GltfImporter::doImage2DLevelCount(const UnsignedInt id) {
    /** @todo remove once the manager-less constructor is gone */
    #ifdef MAGNUM_BUILD_DEPRECATED
    CORRADE_ASSERT(manager(), "Trade::GltfImporter::image2DLevelCount(): the plugin must be instantiated with access to plugin manager in order to open image files", {});
    #endif

    AbstractImporter* importer = setupOrReuseImporterForImage("Trade::GltfImporter::image2DLevelCount():", _d->imagesByDimension[id], 2);
    /* image2DLevelCount() isn't supposed to fail (image2D() is, instead), so
       report 1 on failure and expect image2D() to fail later */
    if(!importer)
        return 1;

    return importer->image2DLevelCount(0);
}

Containers::Optional<ImageData2D> GltfImporter::doImage2D(const UnsignedInt id, const UnsignedInt level) {
    /** @todo remove once the manager-less constructor is gone */
    #ifdef MAGNUM_BUILD_DEPRECATED
    CORRADE_ASSERT(manager(), "Trade::GltfImporter::image2D(): the plugin must be instantiated with access to plugin manager in order to load images", {});
    #endif

    AbstractImporter* importer = setupOrReuseImporterForImage("Trade::GltfImporter::image2D():", _d->imagesByDimension[id], 2);
    if(!importer)
        return {};

    /* Include a pointer to the glTF image in the result */
    Containers::Optional<ImageData2D> imageData = importer->image2D(0, level);
    if(!imageData)
        return Containers::NullOpt;
    return ImageData2D{Utility::move(*imageData), &*_d->gltfImages[id].first()};
}

UnsignedInt GltfImporter::doImage3DCount() const {
    return _d->imagesByDimension.size() - _d->image2DCount;
}

Int GltfImporter::doImage3DForName(const Containers::StringView name) {
    if(!_d->images3DForName) {
        _d->images3DForName.emplace();
        _d->images3DForName->reserve(_d->imagesByDimension.size() - _d->image2DCount);
        for(std::size_t i = _d->image2DCount; i != _d->imagesByDimension.size(); ++i)
            if(const Containers::StringView imageName = _d->gltfImages[_d->imagesByDimension[i]].second())
                _d->images3DForName->emplace(imageName, i - _d->image2DCount);
    }

    const auto found = _d->images3DForName->find(name);
    return found == _d->images3DForName->end() ? -1 : found->second;
}

Containers::String GltfImporter::doImage3DName(const UnsignedInt id) {
    return _d->gltfImages[_d->imagesByDimension[_d->image2DCount + id]].second();
}

UnsignedInt GltfImporter::doImage3DLevelCount(const UnsignedInt id) {
    /** @todo remove once the manager-less constructor is gone */
    #ifdef MAGNUM_BUILD_DEPRECATED
    CORRADE_ASSERT(manager(), "Trade::GltfImporter::image3DLevelCount(): the plugin must be instantiated with access to plugin manager in order to open image files", {});
    #endif

    AbstractImporter* importer = setupOrReuseImporterForImage("Trade::GltfImporter::image3DLevelCount():", _d->imagesByDimension[_d->image2DCount + id], 3);
    /* image3DLevelCount() isn't supposed to fail (image3D() is, instead), so
       report 1 on failure and expect image3D() to fail later */
    if(!importer)
        return 1;

    return importer->image3DLevelCount(0);
}

Containers::Optional<ImageData3D> GltfImporter::doImage3D(const UnsignedInt id, const UnsignedInt level) {
    /** @todo remove once the manager-less constructor is gone */
    #ifdef MAGNUM_BUILD_DEPRECATED
    CORRADE_ASSERT(manager(), "Trade::GltfImporter::image3D(): the plugin must be instantiated with access to plugin manager in order to load images", {});
    #endif

    AbstractImporter* importer = setupOrReuseImporterForImage("Trade::GltfImporter::image3D():", _d->imagesByDimension[_d->image2DCount + id], 3);
    if(!importer)
        return {};

    return importer->image3D(0, level);
}

const void* GltfImporter::doImporterState() const {
    return &*_d->gltf;
}

}}

CORRADE_PLUGIN_REGISTER(GltfImporter, Magnum::Trade::GltfImporter,
    MAGNUM_TRADE_ABSTRACTIMPORTER_PLUGIN_INTERFACE)
