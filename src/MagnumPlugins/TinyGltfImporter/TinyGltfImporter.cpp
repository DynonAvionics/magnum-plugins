/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
                2020, 2021, 2022, 2023, 2024, 2025
              Vladimír Vondruš <mosra@centrum.cz>
    Copyright © 2018 Tobias Stein <stein.tobi@t-online.de>
    Copyright © 2018, 2020 Jonathan Hale <squareys@googlemail.com>

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

#define _MAGNUM_NO_DEPRECATED_TINYGLTFIMPORTER /* So it doesn't yell here */

#include "TinyGltfImporter.h"

#include <cctype>
#include <limits>
#include <unordered_map>
#include <Corrade/Containers/GrowableArray.h>
#include <Corrade/Containers/ArrayTuple.h>
#include <Corrade/Containers/ArrayView.h>
#include <Corrade/Containers/ArrayViewStl.h>
#include <Corrade/Containers/Optional.h>
#include <Corrade/Containers/Pair.h>
#include <Corrade/Containers/StaticArray.h>
#include <Corrade/Utility/Algorithms.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/DebugStl.h> /* for tinygltf errors, attribute names... */
#include <Corrade/Utility/Path.h>
#include <Corrade/Utility/String.h>
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

#define TINYGLTF_IMPLEMENTATION
/* Opt out of tinygltf stb_image dependency */
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
/* Opt out of loading external images */
#define TINYGLTF_NO_EXTERNAL_IMAGE
/* Opt out of filesystem access, as we handle it ourselves. However that makes
   it fail to compile as std::ofstream is not define, so we do that here (and
   newer versions don't seem to fix that either). Enabling filesystem access
   makes the compilation fail on WinRT 2015 because for some reason
   ExpandEnvironmentStringsA() is not defined there. Not sure what is that
   related to since windows.h is included, I assume it's related to the MSVC
   2015 bug where a duplicated templated alias causes the compiler to forget
   random symbols, but I couldn't find anything like that in the codebase.
   Also, this didn't happen before when plugins were built with GL enabled. */
#define TINYGLTF_NO_FS
#include <fstream>

#ifdef CORRADE_TARGET_WINDOWS
/* Tinygltf includes some windows headers, avoid including more than ncessary
   to speed up compilation. WIN32_LEAN_AND_MEAN and NOMINMAX is already defined
   by CMake. */
#define VC_EXTRALEAN
#endif

/* Include like this instead of "MagnumExternal/TinyGltf/tiny_gltf.h" so we can
   include it as a system header and suppress warnings */
#include "tiny_gltf.h"
#ifdef CORRADE_TARGET_WINDOWS
#undef near
#undef far
#endif

namespace Magnum { namespace Trade {

using namespace Magnum::Math::Literals;

namespace {

bool loadImageData(tinygltf::Image* image, const int, std::string*, std::string*, int, int, const unsigned char* data, int size, void*) {
    /* In case the image is an embedded URI, copy its decoded value to the data
       buffer. In all other cases we'll access the referenced buffer or
       external file directly from the doImage2D() implementation. */
    if(image->bufferView == -1 && image->uri.empty())
        image->image.assign(data, data + size);

    return true;
}

std::size_t elementSize(const tinygltf::Accessor& accessor) {
    return tinygltf::GetComponentSizeInBytes(accessor.componentType)*tinygltf::GetNumComponentsInType(accessor.type);
}

const tinygltf::Accessor* checkedAccessor(const tinygltf::Model& model, const char* function, Int id) {
    if(std::size_t(id) >= model.accessors.size()) {
        Error{} << "Trade::TinyGltfImporter::" << Debug::nospace << function << Debug::nospace << "(): accessor" << id << "out of range for" << model.accessors.size() << "accessors";
        return nullptr;
    }

    const tinygltf::Accessor& accessor = model.accessors[id];
    if(accessor.sparse.isSparse) {
        Error{} << "Trade::TinyGltfImporter::" << Debug::nospace << function << Debug::nospace << "(): accessor" << id << "is using sparse storage, which is unsupported";
        return nullptr;
    }
    if(accessor.bufferView < 0) {
        /* Bufferviews are optional in accessors, we're supposed to fill the view
           with zeros. Only makes sense with sparse data and we don't support that. */
        Error{} << "Trade::TinyGltfImporter::" << Debug::nospace << function << Debug::nospace << "(): accessor" << id << "has no bufferView";
        return nullptr;
    }
    if(std::size_t(accessor.bufferView) >= model.bufferViews.size()) {
        Error{} << "Trade::TinyGltfImporter::" << Debug::nospace << function << Debug::nospace << "(): bufferView" << accessor.bufferView << "out of range for" << model.bufferViews.size() << "views";
        return nullptr;
    }

    const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
    const std::size_t size = elementSize(accessor);
    if(bufferView.byteStride != 0 && bufferView.byteStride < size) {
        Error{} << "Trade::TinyGltfImporter::" << Debug::nospace << function << Debug::nospace << "():" << size << Debug::nospace << "-byte type defined by accessor" << id << "can't fit into bufferView" << accessor.bufferView << "stride of" << bufferView.byteStride;
        return nullptr;
    }
    const std::size_t accessorSize = accessor.byteOffset + (accessor.count - 1)*(bufferView.byteStride ? bufferView.byteStride : size) + size;
    if(bufferView.byteLength < accessorSize) {
        Error{} << "Trade::TinyGltfImporter::" << Debug::nospace << function << Debug::nospace << "(): accessor" << id << "needs" << accessorSize << "bytes but bufferView" << accessor.bufferView << "has only" << bufferView.byteLength;
        return nullptr;
    }
    if(std::size_t(bufferView.buffer) >= model.buffers.size()) {
        Error{} << "Trade::TinyGltfImporter::" << Debug::nospace << function << Debug::nospace << "(): buffer" << bufferView.buffer << "out of range for" << model.buffers.size() << "buffers";
        return nullptr;
    }

    const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
    const std::size_t viewSize = bufferView.byteOffset + bufferView.byteLength;
    if(buffer.data.size() < viewSize) {
        Error{} << "Trade::TinyGltfImporter::" << Debug::nospace << function << Debug::nospace << "(): bufferView" << accessor.bufferView << "needs" << viewSize << "bytes but buffer" << bufferView.buffer << "has only" << buffer.data.size();
        return nullptr;
    }

    return &accessor;
}

Containers::StridedArrayView2D<const char> bufferView(const tinygltf::Model& model, const tinygltf::Accessor& accessor) {
    /* All this assumes the accessor was retrieved using checkedAccessor() */
    const std::size_t bufferElementSize = elementSize(accessor);
    const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
    /* Stride could be 0, in which case it's equal to element size */
    const std::size_t stride = bufferView.byteStride ? bufferView.byteStride : bufferElementSize;
    const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

    return Containers::StridedArrayView2D<const char>{Containers::arrayView(buffer.data),
        reinterpret_cast<const char*>(buffer.data.data()) + bufferView.byteOffset + accessor.byteOffset,
        {accessor.count, bufferElementSize},
        {std::ptrdiff_t(stride), 1}};
}

Containers::StringView attributeSemantic(Containers::StringView attribute) {
    /* Get the semantic base name ([semantic]_[set_index]) */
    const Containers::Array3<Containers::StringView> parts = attribute.partition('_');
    const bool isNumbered = !parts.back().isEmpty() &&
        std::all_of(parts.back().begin(), parts.back().end(), [](unsigned char c) { return std::isdigit(c); });
    /* Return empty semantic for invalid or non-numbered attribute names. This
       ensures they don't get recognized as one of the numbered attributes like
       TEXCOORD, etc. just because they share the prefix. */
    return isNumbered ? parts.front() : Containers::StringView{};
}

}

struct TinyGltfImporter::Document {
    Containers::Optional<Containers::String> filePath;

    tinygltf::Model model;

    Containers::Optional<std::unordered_map<std::string, Int>>
        animationsForName,
        camerasForName,
        lightsForName,
        scenesForName,
        skinsForName,
        nodesForName,
        meshesForName,
        materialsForName,
        imagesForName,
        texturesForName;

    /* Unlike the ones above, these are filled already during construction as
       we need them in three different places and on-demand construction would
       be too annoying to test. Also, assuming the importer knows all builtin
       names, in most case these would be empty anyway. */
    std::unordered_map<std::string, MeshAttribute>
        meshAttributesForName;
    Containers::Array<std::string> meshAttributeNames;

    /* Mapping for multi-primitive meshes:

        -   meshMap.size() is the count of meshes reported to the user
        -   meshSizeOffsets.size() is the count of original meshes in the file
            plus one
        -   meshMap[id] is a pair of (original mesh ID, primitive ID)
        -   meshSizeOffsets[j] points to the first item in meshMap for
            original mesh ID `j` -- which also translates the original ID to
            reported ID
        -   meshSizeOffsets[j + 1] - meshSizeOffsets[j] is count of meshes for
            original mesh ID `j` (or number of primitives in given mesh)
    */
    std::vector<std::pair<std::size_t, std::size_t>> meshMap;
    std::vector<std::size_t> meshSizeOffsets;

    /* If a file contains texture coordinates that are not floats or normalized
       in the 0-1, the textureCoordinateYFlipInMaterial option is enabled
       implicitly as we can't perform Y-flip directly on the data. */
    bool textureCoordinateYFlipInMaterial = false;

    bool open = false;

    UnsignedInt imageImporterId = ~UnsignedInt{};
    Containers::Optional<AnyImageImporter> imageImporter;
};

/* LCOV_EXCL_START, the whole plugin is deprecated but direct construction in
   particular as well as it's impossible to cover with tests, so don't have it
   contribute against uncovered lines */
namespace {

void fillDefaultConfiguration(Utility::ConfigurationGroup& conf) {
    conf.setValue("optimizeQuaternionShortestPath", true);
    conf.setValue("normalizeQuaternions", true);
    conf.setValue("mergeAnimationClips", false);
    conf.setValue("phongMaterialFallback", true);
    conf.setValue("objectIdAttribute", "_OBJECT_ID");
}

}

TinyGltfImporter::TinyGltfImporter() {
    fillDefaultConfiguration(configuration());
}

TinyGltfImporter::TinyGltfImporter(PluginManager::Manager<AbstractImporter>& manager): AbstractImporter{manager} {
    fillDefaultConfiguration(configuration());
}
/* LCOV_EXCL_STOP */

TinyGltfImporter::TinyGltfImporter(PluginManager::AbstractManager& manager, const Containers::StringView& plugin): AbstractImporter{manager, plugin} {}

TinyGltfImporter::~TinyGltfImporter() = default;

ImporterFeatures TinyGltfImporter::doFeatures() const { return ImporterFeature::OpenData|ImporterFeature::FileCallback; }

bool TinyGltfImporter::doIsOpened() const { return !!_d && _d->open; }

void TinyGltfImporter::doClose() { _d = nullptr; }

void TinyGltfImporter::doOpenFile(const Containers::StringView filename) {
    _d.reset(new Document);
    /* Since the slice won't be null terminated, nullTerminatedGlobalView()
       won't help anything here */
    _d->filePath.emplace(Utility::Path::path(filename));
    AbstractImporter::doOpenFile(filename);
}

void TinyGltfImporter::doOpenData(Containers::Array<char>&& data, DataFlags) {
    tinygltf::TinyGLTF loader;
    std::string err;

    if(!_d) _d.reset(new Document);

    /* Set up file callbacks */
    tinygltf::FsCallbacks callbacks;
    callbacks.user_data = this;
    /* We don't need any expansion of environment variables in file paths.
       That should be done in a completely different place and is not something
       the importer should care about. Further, FileExists and ExpandFilePath
       is used to search for files in a few different locations. That's also
       totally useless, since location of dependent files is *clearly* and
       uniquely defined. Also, tinygltf's path joining is STUPID and so
       /foo/bar/ + /file.dat gets joined to /foo/bar//file.dat. So we supply
       an empty path there and handle it here correctly. */
    callbacks.FileExists = [](const std::string&, void*) { return true; };
    callbacks.ExpandFilePath = [](const std::string& path, void*) {
        return path;
    };
    if(fileCallback()) callbacks.ReadWholeFile = [](std::vector<unsigned char>* out, std::string* err, const std::string& filename, void* userData) {
            CORRADE_IGNORE_DEPRECATED_PUSH /* MSVC, FUCK OFF, why you warn here */
            auto& self = *static_cast<TinyGltfImporter*>(userData);
            const Containers::String fullPath = Utility::Path::join(self._d->filePath ? *self._d->filePath : "", filename);
            CORRADE_IGNORE_DEPRECATED_POP
            Containers::Optional<Containers::ArrayView<const char>> data = self.fileCallback()(fullPath, InputFileCallbackPolicy::LoadTemporary, self.fileCallbackUserData());
            if(!data) {
                *err = "file callback failed";
                return false;
            }
            out->assign(data->begin(), data->end());
            return true;
        };
    else callbacks.ReadWholeFile = [](std::vector<unsigned char>* out, std::string* err, const std::string& filename, void* userData) {
            CORRADE_IGNORE_DEPRECATED_PUSH /* MSVC, FUCK OFF, why you warn here */
            auto& self = *static_cast<TinyGltfImporter*>(userData);
            if(!self._d->filePath) {
                *err = "external buffers can be imported only when opening files from the filesystem or if a file callback is present";
                return false;
            }
            const Containers::String fullPath = Utility::Path::join(*self._d->filePath, filename);
            CORRADE_IGNORE_DEPRECATED_POP
            Containers::Optional<Containers::Array<char>> data = Utility::Path::read(fullPath);
            if(!data) {
                *err = "file reading failed";
                return false;
            }
            out->assign(data->begin(), data->end());
            return true;
        };
    /* This field is not used (we're just importing here) but GCC 10 warns
       about a "maybe uninitialized" variable otherwise. */
    callbacks.WriteWholeFile = nullptr;
    loader.SetFsCallbacks(callbacks);

    loader.SetImageLoader(&loadImageData, nullptr);

    _d->open = true;
    if(data.size() >= 4 && strncmp(data.data(), "glTF", 4) == 0) {
        _d->open = loader.LoadBinaryFromMemory(&_d->model, &err, nullptr, reinterpret_cast<const unsigned char*>(data.data()), data.size(), "", tinygltf::SectionCheck::NO_REQUIRE);
    } else {
        _d->open = loader.LoadASCIIFromString(&_d->model, &err, nullptr, data.data(), data.size(), "", tinygltf::SectionCheck::NO_REQUIRE);
    }

    if(!_d->open) {
        Utility::String::rtrimInPlace(err);
        Error{} << "Trade::TinyGltfImporter::openData(): error opening file:" << err;
        doClose();
        return;
    }

    /* Major versions are forward- and backward-compatible, but minVersion can
       be used to require support for features added in new minor versions.
       So far there's only 2.0 so we can use an exact comparison. */
    const tinygltf::Asset& asset = _d->model.asset;
    if(!asset.minVersion.empty() && asset.minVersion != "2.0") {
        Error{} << "Trade::TinyGltfImporter::openData(): unsupported minVersion" << asset.minVersion << Debug::nospace << ", expected 2.0";
        doClose();
        return;
    }
    if(!asset.version.empty() && asset.version.find("2.") != 0) {
        Error{} << "Trade::TinyGltfImporter::openData(): unsupported version" << asset.version << Debug::nospace << ", expected 2.x";
        doClose();
        return;
    }

    /* Bounds checks that can't be deferred to later. No, tinygltf doesn't
       check for this. */
    if(_d->model.defaultScene != -1 && UnsignedInt(_d->model.defaultScene) >= _d->model.scenes.size()) {
        Error{} << "Trade::TinyGltfImporter::openData(): scene index" << _d->model.defaultScene << "out of range for" << _d->model.scenes.size() << "scenes";
        doClose();
        return;
    }

    /* Check node hierarchy for forbidden parent-child relationships and
       non-root scene nodes */

    Containers::Array<Int> parentFor{DirectInit, _d->model.nodes.size(), -1};
    for(std::size_t i = 0; i != _d->model.nodes.size(); ++i) {
        for(Int child: _d->model.nodes[i].children) {
            /* Ignore out-of-bounds child nodes, those produce an error later
               in doObject3D() */
            if(UnsignedInt(child) >= _d->model.nodes.size())
                continue;

            if(parentFor[child] != -1) {
                Error{} << "Trade::TinyGltfImporter::openData(): node" << child << "has multiple parents";
                doClose();
                return;
            }
            parentFor[child] = i;
        }
    }

    for(std::size_t i = 0; i != _d->model.scenes.size(); ++i) {
        for(Int node: _d->model.scenes[i].nodes) {
            /* Ignore out-of-bounds scene nodes, those produce an error later
               in doScene() */
            if(UnsignedInt(node) >= _d->model.nodes.size())
                continue;

            if(parentFor[node] != -1) {
                Error{} << "Trade::TinyGltfImporter::openData(): node" << node << "in scene" << i << "is not a root node";
                doClose();
                return;
            }
        }
    }

    for(std::size_t i = 0; i != _d->model.nodes.size(); ++i) {
        auto getParent = [&](Int node) -> Int {
            return node != -1 ? parentFor[node] : -1;
        };

        Int p1 = getParent(i);
        Int p2 = getParent(p1);

        while(p1 != -1 && p2 != -1) {
            if(p1 == p2) {
                Error{} << "Trade::TinyGltfImporter::openData(): node tree contains cycle starting at node" << i;
                doClose();
                return;
            }

            p1 = getParent(p1);
            p2 = getParent(getParent(p2));
        }
    }

    /* Treat meshes with multiple primitives as separate meshes. Each mesh gets
       duplicated as many times as is the size of the primitives array. */
    _d->meshSizeOffsets.emplace_back(0);
    for(std::size_t i = 0; i != _d->model.meshes.size(); ++i) {
        CORRADE_INTERNAL_ASSERT(!_d->model.meshes[i].primitives.empty());
        for(std::size_t j = 0; j != _d->model.meshes[i].primitives.size(); ++j)
            _d->meshMap.emplace_back(i, j);

        _d->meshSizeOffsets.emplace_back(_d->meshMap.size());
    }

    /* Go through all meshes, collect custom attributes and decide about
       implicitly enabling textureCoordinateYFlipInMaterial if it isn't already
       requested from the configuration and there are any texture coordinates
       that need it */
    if(configuration().value<bool>("textureCoordinateYFlipInMaterial"))
        _d->textureCoordinateYFlipInMaterial = true;
    for(const tinygltf::Mesh& mesh: _d->model.meshes) {
        for(const tinygltf::Primitive& primitive: mesh.primitives) {
            for(const std::pair<const std::string, int>& attribute: primitive.attributes) {
                const Containers::StringView semantic = attributeSemantic(attribute.first);
                if(semantic == "TEXCOORD") {
                    if(!_d->textureCoordinateYFlipInMaterial) {
                        /* Ignore accessor is out of range, this will fail
                           later during mesh import */
                        if(std::size_t(attribute.second) >= _d->model.accessors.size())
                            continue;

                        const int type = _d->model.accessors[attribute.second].componentType;
                        const bool normalized = _d->model.accessors[attribute.second].normalized;
                        if(type == TINYGLTF_COMPONENT_TYPE_BYTE ||
                           type == TINYGLTF_COMPONENT_TYPE_SHORT ||
                          (type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE && !normalized) ||
                          (type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT && !normalized)) {
                            Debug{} << "Trade::TinyGltfImporter::openData(): file contains non-normalized texture coordinates, implicitly enabling textureCoordinateYFlipInMaterial";
                            _d->textureCoordinateYFlipInMaterial = true;
                        }
                    }

                /* If the name isn't recognized or not in MeshAttribute, add
                   the attribute to custom if not there already */
                } else if(attribute.first != "POSITION" &&
                    attribute.first != "NORMAL" &&
                    attribute.first != "TANGENT" &&
                    semantic != "COLOR" &&
                    /* Reported as MeshAttribute::ObjectId */
                    attribute.first != configuration().value("objectIdAttribute"))
                {
                    if(_d->meshAttributesForName.emplace(attribute.first,
                        meshAttributeCustom(_d->meshAttributeNames.size())).second)
                        arrayAppend(_d->meshAttributeNames, attribute.first);
                }
            }
        }
    }

    /* Name maps are lazy-loaded because these might not be needed every time */
}

UnsignedInt TinyGltfImporter::doCameraCount() const {
    return _d->model.cameras.size();
}

Int TinyGltfImporter::doCameraForName(const Containers::StringView name) {
    if(!_d->camerasForName) {
        _d->camerasForName.emplace();
        _d->camerasForName->reserve(_d->model.cameras.size());
        for(std::size_t i = 0; i != _d->model.cameras.size(); ++i)
            _d->camerasForName->emplace(_d->model.cameras[i].name, i);
    }

    const auto found = _d->camerasForName->find(name);
    return found == _d->camerasForName->end() ? -1 : found->second;
}

Containers::String TinyGltfImporter::doCameraName(const UnsignedInt id) {
    return _d->model.cameras[id].name;
}

UnsignedInt TinyGltfImporter::doAnimationCount() const {
    /* If the animations are merged, there's at most one */
    if(configuration().value<bool>("mergeAnimationClips"))
        return _d->model.animations.empty() ? 0 : 1;

    return _d->model.animations.size();
}

Int TinyGltfImporter::doAnimationForName(const Containers::StringView name) {
    /* If the animations are merged, don't report any names */
    if(configuration().value<bool>("mergeAnimationClips")) return -1;

    if(!_d->animationsForName) {
        _d->animationsForName.emplace();
        _d->animationsForName->reserve(_d->model.animations.size());
        for(std::size_t i = 0; i != _d->model.animations.size(); ++i)
            _d->animationsForName->emplace(_d->model.animations[i].name, i);
    }

    const auto found = _d->animationsForName->find(name);
    return found == _d->animationsForName->end() ? -1 : found->second;
}

Containers::String TinyGltfImporter::doAnimationName(UnsignedInt id) {
    /* If the animations are merged, don't report any names */
    if(configuration().value<bool>("mergeAnimationClips")) return {};
    return _d->model.animations[id].name;
}

namespace {

template<class V> void postprocessSplineTrack(const std::size_t timeTrackUsed, const Containers::ArrayView<const Float> keys, const Containers::ArrayView<Math::CubicHermite<V>> values) {
    /* Already processed, don't do that again */
    if(timeTrackUsed != ~std::size_t{}) return;

    CORRADE_INTERNAL_ASSERT(keys.size() == values.size());
    if(keys.size() < 2) return;

    /* Convert the `a` values to `n` and the `b` values to `m` as described in
       https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#appendix-c-spline-interpolation
       Unfortunately I was not able to find any concrete name for this, so it's
       not part of the CubicHermite implementation but is kept here locally. */
    for(std::size_t i = 0; i < keys.size() - 1; ++i) {
        const Float timeDifference = keys[i + 1] - keys[i];
        values[i].outTangent() *= timeDifference;
        values[i + 1].inTangent() *= timeDifference;
    }
}

}

Containers::Optional<AnimationData> TinyGltfImporter::doAnimation(UnsignedInt id) {
    /* Import either a single animation or all of them together. At the moment,
       Blender doesn't really support cinematic animations (affecting multiple
       objects): https://blender.stackexchange.com/q/5689. And since
       https://github.com/KhronosGroup/glTF-Blender-Exporter/pull/166, these
       are exported as a set of object-specific clips, which may not be wanted,
       so we give the users an option to merge them all together. */
    const std::size_t animationBegin =
        configuration().value<bool>("mergeAnimationClips") ? 0 : id;
    const std::size_t animationEnd =
        configuration().value<bool>("mergeAnimationClips") ? _d->model.animations.size() : id + 1;

    /* First gather the input and output data ranges. Key is unique accessor ID
       so we don't duplicate shared data, value is range in the input buffer,
       offset in the output data and ID of the corresponding key track in case
       given track is a spline interpolation. The key ID is initialized to ~0
       and will be used later to check that a spline track was not used with
       more than one time track, as it needs to be postprocessed for given time
       track. */
    std::unordered_map<int, std::tuple<Containers::StridedArrayView2D<const char>, std::size_t, std::size_t>> samplerData;
    std::size_t dataSize = 0;
    for(std::size_t a = animationBegin; a != animationEnd; ++a) {
        const tinygltf::Animation& animation = _d->model.animations[a];
        for(std::size_t i = 0; i != animation.samplers.size(); ++i) {
            const tinygltf::AnimationSampler& sampler = animation.samplers[i];

            const tinygltf::Accessor* input = checkedAccessor(_d->model, "animation", sampler.input);
            if(!input) return Containers::NullOpt;

            const tinygltf::Accessor* output = checkedAccessor(_d->model, "animation", sampler.output);
            if(!output) return Containers::NullOpt;

            /** @todo handle alignment once we do more than just four-byte types */

            /* If the input view is not yet present in the output data buffer,
               add it */
            if(samplerData.find(sampler.input) == samplerData.end()) {
                Containers::StridedArrayView2D<const char> view = bufferView(_d->model, *input);
                samplerData.emplace(sampler.input, std::make_tuple(view, dataSize, ~std::size_t{}));
                dataSize += view.size()[0]*view.size()[1];
            }

            /* If the output view is not yet present in the output data buffer,
               add it */
            if(samplerData.find(sampler.output) == samplerData.end()) {
                Containers::StridedArrayView2D<const char> view = bufferView(_d->model, *output);
                samplerData.emplace(sampler.output, std::make_tuple(view, dataSize, ~std::size_t{}));
                dataSize += view.size()[0]*view.size()[1];
            }
        }
    }

    /* Populate the data array */
    /**
     * @todo Once memory-mapped files are supported, this can all go away
     *      except when spline tracks are present -- in that case we need to
     *      postprocess them and can't just use the memory directly.
     */
    Containers::Array<char> data{dataSize};
    for(const std::pair<const int, std::tuple<Containers::StridedArrayView2D<const char>, std::size_t, std::size_t>>& view: samplerData) {
        Containers::StridedArrayView2D<const char> src;
        std::size_t outputOffset;
        std::tie(src, outputOffset, std::ignore) = view.second;

        Containers::StridedArrayView2D<char> dst{data.exceptPrefix(outputOffset),
            src.size()};
        Utility::copy(src, dst);
    }

    /* Calculate total track count. If merging all animations together, this is
       the sum of all clip track counts. */
    std::size_t trackCount = 0;
    for(std::size_t a = animationBegin; a != animationEnd; ++a)
        trackCount += _d->model.animations[a].channels.size();

    /* Import all tracks */
    bool hadToRenormalize = false;
    std::size_t trackId = 0;
    Containers::Array<Trade::AnimationTrackData> tracks{trackCount};
    for(std::size_t a = animationBegin; a != animationEnd; ++a) {
        const tinygltf::Animation& animation = _d->model.animations[a];
        for(std::size_t i = 0; i != animation.channels.size(); ++i) {
            const tinygltf::AnimationChannel& channel = animation.channels[i];
            if(std::size_t(channel.sampler) >= animation.samplers.size()) {
                Error{} << "Trade::TinyGltfImporter::animation(): sampler" << channel.sampler << "out of range for" << animation.samplers.size() << "samplers";
                return Containers::NullOpt;
            }

            const tinygltf::AnimationSampler& sampler = animation.samplers[channel.sampler];

            /* Key properties -- always float time. Not using checkedAccessor()
               as this was all checked above once already. */
            const tinygltf::Accessor& input = _d->model.accessors[sampler.input];
            if(input.type != TINYGLTF_TYPE_SCALAR || input.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
                Error{} << "Trade::TinyGltfImporter::animation(): time track has unexpected type" << input.type << Debug::nospace << "/" << Debug::nospace << input.componentType;
                return Containers::NullOpt;
            }

            /* View on the key data */
            const auto inputDataFound = samplerData.find(sampler.input);
            CORRADE_INTERNAL_ASSERT(inputDataFound != samplerData.end());
            const auto keys = Containers::arrayCast<Float>(
                data.exceptPrefix(std::get<1>(inputDataFound->second)).prefix(
                    std::get<0>(inputDataFound->second).size()[0]*
                    std::get<0>(inputDataFound->second).size()[1]));

            /* Interpolation mode */
            Animation::Interpolation interpolation;
            if(sampler.interpolation == "LINEAR") {
                interpolation = Animation::Interpolation::Linear;
            } else if(sampler.interpolation == "CUBICSPLINE") {
                interpolation = Animation::Interpolation::Spline;
            } else if(sampler.interpolation == "STEP") {
                interpolation = Animation::Interpolation::Constant;
            } else {
                Error{} << "Trade::TinyGltfImporter::animation(): unsupported interpolation" << sampler.interpolation;
                return Containers::NullOpt;
            }

            /* Decide on value properties. Not using checkedAccessor() as this
               was all checked above once already. */
            const tinygltf::Accessor& output = _d->model.accessors[sampler.output];
            AnimationTrackTarget target;
            AnimationTrackType type, resultType;
            Animation::TrackViewStorage<const Float> track;
            const auto outputDataFound = samplerData.find(sampler.output);
            CORRADE_INTERNAL_ASSERT(outputDataFound != samplerData.end());
            const auto outputData = data.exceptPrefix(std::get<1>(outputDataFound->second))
                .prefix(std::get<0>(outputDataFound->second).size()[0]*
                        std::get<0>(outputDataFound->second).size()[1]);
            std::size_t& timeTrackUsed = std::get<2>(outputDataFound->second);

            /* Translation */
            if(channel.target_path == "translation") {
                if(output.type != TINYGLTF_TYPE_VEC3 || output.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
                    Error{} << "Trade::TinyGltfImporter::animation(): translation track has unexpected type" << output.type << Debug::nospace << "/" << Debug::nospace << output.componentType;
                    return Containers::NullOpt;
                }

                /* View on the value data */
                target = AnimationTrackTarget::Translation3D;
                resultType = AnimationTrackType::Vector3;
                if(interpolation == Animation::Interpolation::Spline) {
                    /* Postprocess the spline track. This can be done only once
                       for every track -- postprocessSplineTrack() checks
                       that. */
                    const auto values = Containers::arrayCast<CubicHermite3D>(outputData);
                    postprocessSplineTrack(timeTrackUsed, keys, values);

                    type = AnimationTrackType::CubicHermite3D;
                    track = Animation::TrackView<const Float, const CubicHermite3D>{
                        keys, values, interpolation,
                        animationInterpolatorFor<CubicHermite3D>(interpolation),
                        Animation::Extrapolation::Constant};
                } else {
                    type = AnimationTrackType::Vector3;
                    track = Animation::TrackView<const Float, const Vector3>{keys,
                        Containers::arrayCast<Vector3>(outputData),
                        interpolation,
                        animationInterpolatorFor<Vector3>(interpolation),
                        Animation::Extrapolation::Constant};
                }

            /* Rotation */
            } else if(channel.target_path == "rotation") {
                /** @todo rotation can be also normalized (?!) to a vector of 8/16/32bit (signed?!) integers */

                if(output.type != TINYGLTF_TYPE_VEC4 || output.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
                    Error{} << "Trade::TinyGltfImporter::animation(): rotation track has unexpected type" << output.type << Debug::nospace << "/" << Debug::nospace << output.componentType;
                    return Containers::NullOpt;
                }

                /* View on the value data */
                target = AnimationTrackTarget::Rotation3D;
                resultType = AnimationTrackType::Quaternion;
                if(interpolation == Animation::Interpolation::Spline) {
                    /* Postprocess the spline track. This can be done only once
                       for every track -- postprocessSplineTrack() checks
                       that. */
                    const auto values = Containers::arrayCast<CubicHermiteQuaternion>(outputData);
                    postprocessSplineTrack(timeTrackUsed, keys, values);

                    type = AnimationTrackType::CubicHermiteQuaternion;
                    track = Animation::TrackView<const Float, const CubicHermiteQuaternion>{
                        keys, values, interpolation,
                        animationInterpolatorFor<CubicHermiteQuaternion>(interpolation),
                        Animation::Extrapolation::Constant};
                } else {
                    /* Ensure shortest path is always chosen. Not doing this
                       for spline interpolation, there it would cause war and
                       famine. */
                    const auto values = Containers::arrayCast<Quaternion>(outputData);
                    if(configuration().value<bool>("optimizeQuaternionShortestPath")) {
                        Float flip = 1.0f;
                        for(std::size_t j = 0; j + 1 < values.size(); ++j) {
                            if(Math::dot(values[j], values[j + 1]*flip) < 0) flip = -flip;
                            values[j + 1] *= flip;
                        }
                    }

                    /* Normalize the quaternions if not already. Don't attempt
                       to normalize every time to avoid tiny differences, only
                       when the quaternion looks to be off. Again, not doing
                       this for splines as it would cause things to go
                       haywire. */
                    if(configuration().value<bool>("normalizeQuaternions")) {
                        for(auto& j: values) if(!j.isNormalized()) {
                            j = j.normalized();
                            hadToRenormalize = true;
                        }
                    }

                    type = AnimationTrackType::Quaternion;
                    track = Animation::TrackView<const Float, const Quaternion>{
                        keys, values, interpolation,
                        animationInterpolatorFor<Quaternion>(interpolation),
                        Animation::Extrapolation::Constant};
                }

            /* Scale */
            } else if(channel.target_path == "scale") {
                if(output.type != TINYGLTF_TYPE_VEC3 || output.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
                    Error{} << "Trade::TinyGltfImporter::animation(): scaling track has unexpected type" << output.type << Debug::nospace << "/" << Debug::nospace << output.componentType;
                    return Containers::NullOpt;
                }

                /* View on the value data */
                target = AnimationTrackTarget::Scaling3D;
                resultType = AnimationTrackType::Vector3;
                if(interpolation == Animation::Interpolation::Spline) {
                    /* Postprocess the spline track. This can be done only once
                       for every track -- postprocessSplineTrack() checks
                       that. */
                    const auto values = Containers::arrayCast<CubicHermite3D>(outputData);
                    postprocessSplineTrack(timeTrackUsed, keys, values);

                    type = AnimationTrackType::CubicHermite3D;
                    track = Animation::TrackView<const Float, const CubicHermite3D>{
                        keys, values, interpolation,
                        animationInterpolatorFor<CubicHermite3D>(interpolation),
                        Animation::Extrapolation::Constant};
                } else {
                    type = AnimationTrackType::Vector3;
                    track = Animation::TrackView<const Float, const Vector3>{keys,
                        Containers::arrayCast<Vector3>(outputData),
                        interpolation,
                        animationInterpolatorFor<Vector3>(interpolation),
                        Animation::Extrapolation::Constant};
                }

            } else {
                Error{} << "Trade::TinyGltfImporter::animation(): unsupported track target" << channel.target_path;
                return Containers::NullOpt;
            }

            /* Splines were postprocessed using the corresponding time track.
               If a spline is not yet marked as postprocessed, mark it.
               Otherwise check that the spline track is always used with the
               same time track. */
            if(interpolation == Animation::Interpolation::Spline) {
                if(timeTrackUsed == ~std::size_t{})
                    timeTrackUsed = sampler.input;
                else if(timeTrackUsed != std::size_t(sampler.input)) {
                    Error{} << "Trade::TinyGltfImporter::animation(): spline track is shared with different time tracks, we don't support that, sorry";
                    return Containers::NullOpt;
                }
            }

            if(std::size_t(channel.target_node) >= _d->model.nodes.size()) {
                Error{} << "Trade::TinyGltfImporter::animation(): target node" << channel.target_node << "out of range for" << _d->model.nodes.size() << "nodes";
                return Containers::NullOpt;
            }

            /* As the whole plugin is deprecated, it's not worth wasting time
               with porting to non-deprecated APIs */
            CORRADE_IGNORE_DEPRECATED_PUSH
            tracks[trackId++] = AnimationTrackData{type, resultType, target,
                UnsignedInt(channel.target_node),
                track};
            CORRADE_IGNORE_DEPRECATED_POP
        }
    }

    if(hadToRenormalize)
        Warning{} << "Trade::TinyGltfImporter::animation(): quaternions in some rotation tracks were renormalized";

    return AnimationData{std::move(data), std::move(tracks),
        configuration().value<bool>("mergeAnimationClips") ? nullptr :
        &_d->model.animations[id]};
}

Containers::Optional<CameraData> TinyGltfImporter::doCamera(UnsignedInt id) {
    const tinygltf::Camera& camera = _d->model.cameras[id];

    /* https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#projection-matrices */

    /* Perspective camera. glTF uses vertical FoV and X/Y aspect ratio, so to
       avoid accidental bugs we will directly calculate the near plane size and
       use that to create the camera data (instead of passing it the horizontal
       FoV). Also tinygltf is stupid and uses 0 to denote infinite far plane
       (wat). */
    if(camera.type == "perspective") {
        const Vector2 size = 2.0f*Float(camera.perspective.znear)*Math::tan(Float(camera.perspective.yfov)*0.5_radf)*Vector2::xScale(Float(camera.perspective.aspectRatio));
        const Float far = Float(camera.perspective.zfar) == 0.0f ?
            Constants::inf() : Float(camera.perspective.zfar);
        return CameraData{CameraType::Perspective3D, size, Float(camera.perspective.znear), far, &camera};
    }

    /* Orthographic camera. glTF uses a "scale" instead of "size", which means
       we have to double. */
    if(camera.type == "orthographic")
        return CameraData{CameraType::Orthographic3D,
            Vector2{Float(camera.orthographic.xmag), Float(camera.orthographic.ymag)}*2.0f,
            Float(camera.orthographic.znear), Float(camera.orthographic.zfar), &camera};

    CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
}

UnsignedInt TinyGltfImporter::doLightCount() const {
    return _d->model.lights.size();
}

Int TinyGltfImporter::doLightForName(const Containers::StringView name) {
    if(!_d->lightsForName) {
        _d->lightsForName.emplace();
        _d->lightsForName->reserve(_d->model.lights.size());
        for(std::size_t i = 0; i != _d->model.lights.size(); ++i)
            _d->lightsForName->emplace(_d->model.lights[i].name, i);
    }

    const auto found = _d->lightsForName->find(name);
    return found == _d->lightsForName->end() ? -1 : found->second;
}

Containers::String TinyGltfImporter::doLightName(const UnsignedInt id) {
    return _d->model.lights[id].name;
}

Containers::Optional<LightData> TinyGltfImporter::doLight(UnsignedInt id) {
    const tinygltf::Light& light = _d->model.lights[id];

    /* Light type */
    LightType type;
    if(light.type == "point") {
        type = LightType::Point;
    } else if(light.type == "spot") {
        type = LightType::Spot;
    } else if(light.type == "directional") {
        type = LightType::Directional;
    } else {
        Error{} << "Trade::TinyGltfImporter::light(): invalid light type" << light.type;
        return Containers::NullOpt;
    }

    /* Light color */
    Color3 color{NoInit};
    if(light.color.size() == 3)
        color = {Float(light.color[0]), Float(light.color[1]), Float(light.color[2])};
    else if(light.color.size() == 0)
        color = Color3{1.0f};
    else {
        Error{} << "Trade::TinyGltfImporter::light(): expected three values for a color, got" << light.color.size();
        return Containers::NullOpt;
    }

    /* Spotlight cone angles. In glTF they're specified as half-angles (which
       is also why the limit on outer angle is 90°, not 180°), to avoid
       confusion report a potential error in the original half-angles and
       double the angle only at the end. */
    Rad innerConeAngle{NoInit}, outerConeAngle{NoInit};
    if(type == LightType::Spot) {
        innerConeAngle = Rad{Float(light.spot.innerConeAngle)};
        outerConeAngle = Rad{Float(light.spot.outerConeAngle)};

        if(innerConeAngle < Rad(0.0_degf) || innerConeAngle >= outerConeAngle || outerConeAngle >= Rad(90.0_degf)) {
            Error{} << "Trade::TinyGltfImporter::light(): inner and outer cone angle" << Deg(innerConeAngle) << "and" << Deg(outerConeAngle) << "out of allowed bounds";
            return Containers::NullOpt;
        }
    } else innerConeAngle = outerConeAngle = 180.0_degf;

    /* Tinygltf sets range to 0 instead of infinity when it's not present.
       That's stupid because it would divide by zero, fix that. Even more
       stupid is JSON not having ANY way to represent an infinity, FFS. */
    Float range;
    if(light.range == 0.0) range = Constants::inf();
    else range = Float(light.range);

    /* Range should be infinity for directional lights. Because there's no way
       to represent infinity in JSON, directly suggest to remove the range
       property, don't even bother printing the value. */
    if(type == LightType::Directional && range != Constants::inf()) {
        Error{} << "Trade::TinyGltfImporter::light(): range can't be defined for a directional light";
        return Containers::NullOpt;
    }

    /* As said above, glTF uses half-angles, while we have full angles (for
       consistency with existing APIs such as OpenAL cone angles or math intersection routines as well as Blender). */
    return LightData{type, color, Float(light.intensity), range, innerConeAngle*2.0f, outerConeAngle*2.0f, &light};
}

Int TinyGltfImporter::doDefaultScene() const {
    /* While https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#scenes
       says that "When scene is undefined, client implementations MAY delay
       rendering until a particular scene is requested.", several official
       sample glTF models (e.g. the AnimatedTriangle) have no "scene" property,
       so that's a bit stupid behavior to have. As per discussion at
       https://github.com/KhronosGroup/glTF/issues/815#issuecomment-274286889,
       if a default scene isn't defined and there is at least one scene, just
       use the first one. */
    if(_d->model.defaultScene == -1 && !_d->model.scenes.empty())
        return 0;

    /* Bounds-checked in doOpenData() */
    return _d->model.defaultScene;
}

UnsignedInt TinyGltfImporter::doSceneCount() const { return _d->model.scenes.size(); }

Int TinyGltfImporter::doSceneForName(const Containers::StringView name) {
    if(!_d->scenesForName) {
        _d->scenesForName.emplace();
        _d->scenesForName->reserve(_d->model.scenes.size());
        for(std::size_t i = 0; i != _d->model.scenes.size(); ++i)
            _d->scenesForName->emplace(_d->model.scenes[i].name, i);
    }

    const auto found = _d->scenesForName->find(name);
    return found == _d->scenesForName->end() ? -1 : found->second;
}

Containers::String TinyGltfImporter::doSceneName(const UnsignedInt id) {
    return _d->model.scenes[id].name;
}

Containers::Optional<SceneData> TinyGltfImporter::doScene(UnsignedInt id) {
    const tinygltf::Scene& scene = _d->model.scenes[id];

    /* Gather all top-level nodes belonging to a scene and recursively populate
       the children ranges. Optimistically assume the glTF has just a single
       scene and reserve for that. */
    /** @todo once we have BitArrays use the objects array to mark nodes that
        are present in the scene and then create a new array from those but
        ordered so we can have OrderedMapping for parents and also all other
        fields */
    Containers::Array<UnsignedInt> objects;
    arrayReserve(objects, _d->model.nodes.size());
    for(const UnsignedInt i: scene.nodes) {
        if(i >= _d->model.nodes.size()) {
            Error{} << "Trade::TinyGltfImporter::scene(): node index" << i << "out of range for" << _d->model.nodes.size() << "nodes";
            return {};
        }

        arrayAppend(objects, i);
    }

    Containers::Array<Range1Dui> children;
    arrayReserve(children, _d->model.nodes.size() + 1);
    arrayAppend(children, InPlaceInit, 0u, UnsignedInt(objects.size()));
    for(std::size_t i = 0; i != children.size(); ++i) {
        const Range1Dui& nodeRangeToProcess = children[i];
        for(std::size_t j = nodeRangeToProcess.min(); j != nodeRangeToProcess.max(); ++j) {
            arrayAppend(children, InPlaceInit, UnsignedInt(objects.size()), UnsignedInt(objects.size() + _d->model.nodes[objects[j]].children.size()));
            for(const UnsignedInt k: _d->model.nodes[objects[j]].children) {
                if(k >= _d->model.nodes.size()) {
                    Error{} << "Trade::TinyGltfImporter::scene(): child index" << k << "in node" << objects[j] << "out of range for" << _d->model.nodes.size() << "nodes";
                    return {};
                }

                arrayAppend(objects, k);
            }
        }
    }

    /** @todo once there's SceneData::mappingRange(), calculate also min here */
    const UnsignedInt maxObjectIndex = Math::max(objects);

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
    for(const UnsignedInt i: objects) {
        const tinygltf::Node& node = _d->model.nodes[i];

        /* Everything that has a TRS should have a transformation matrix as
           well. OTOH there can be a transformation matrix but no TRS, and
           there can also be objects without any transformation. */
        if(node.translation.size() != 0 ||
           node.rotation.size() != 0 ||
           node.scale.size() != 0) {
            ++trsCount;
            ++transformationCount;
        } else if(node.matrix.size() != 0) ++transformationCount;

        if(node.translation.size() != 0) hasTranslations = true;
        if(node.rotation.size() != 0) hasRotations = true;
        if(node.scale.size() != 0) hasScalings = true;
        if(node.mesh != -1) {
            if(UnsignedInt(node.mesh) >= _d->model.meshes.size()) {
                Error{} << "Trade::TinyGltfImporter::scene(): mesh index" << node.mesh << "in node" << i << "out of range for" << _d->model.meshes.size() << "meshes";
                return {};
            }

            const tinygltf::Mesh& mesh = _d->model.meshes[node.mesh];
            meshCount += mesh.primitives.size();
            for(const tinygltf::Primitive& primitive: mesh.primitives) {
                if(primitive.material != -1) {
                    hasMeshMaterials = true;
                    break;
                }
            }
        }
        if(node.camera != -1) ++cameraCount;
        if(node.skin != -1) ++skinCount;
        if(node.extensions.find("KHR_lights_punctual") != node.extensions.end()) {
            ++lightCount;
        }
    }

    /* If all objects that have transformations have TRS as well, no need to
       store the combined transform field */
    if(trsCount == transformationCount) transformationCount = 0;

    /* Allocate the output array */
    Containers::ArrayView<UnsignedInt> parentImporterStateObjects;
    Containers::ArrayView<Int> parents;
    Containers::ArrayView<const void*> importerState;
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
        {NoInit, skinCount, skins}
    };

    /* Populate object mapping for parents and importer state, synthesize
       parent info from the child ranges */
    Utility::copy(objects, parentImporterStateObjects);
    for(std::size_t i = 0; i != children.size(); ++i) {
        Int parent = Int(i) - 1;
        for(std::size_t j = children[i].min(); j != children[i].max(); ++j)
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
        const tinygltf::Node& node = _d->model.nodes[objects[i]];

        /* Populate importer state */
        importerState[i] = &node;

        /* Parse TRS */
        Vector3 translation;
        if(node.translation.size() != 0) {
            CORRADE_INTERNAL_ASSERT(node.translation.size() == 3);
            translation = Vector3{Vector3d::from(node.translation.data())};
        }

        Quaternion rotation;
        if(node.rotation.size() != 0) {
            CORRADE_INTERNAL_ASSERT(node.rotation.size() == 4);
            rotation = Quaternion{Vector3{Vector3d::from(node.rotation.data())}, Float(node.rotation[3])};
            if(!rotation.isNormalized() && configuration().value<bool>("normalizeQuaternions")) {
                rotation = rotation.normalized();
                Warning{} << "Trade::TinyGltfImporter::scene(): rotation quaternion of node" << objects[i] << "was renormalized";
            }
        }

        Vector3 scaling{1.0f};
        if(node.scale.size() != 0) {
            CORRADE_INTERNAL_ASSERT(node.scale.size() == 3);
            scaling = Vector3{Vector3d::from(node.scale.data())};
        }

        /* Parse transformation, or combine it from TRS if not present */
        Matrix4 transformation;
        if(node.matrix.size() != 0) {
            CORRADE_INTERNAL_ASSERT(node.matrix.size() == 16);
            transformation = Matrix4{Matrix4d::from(node.matrix.data())};
        } else {
            transformation =
                Matrix4::translation(translation)*
                Matrix4{rotation.toMatrix()}*
                Matrix4::scaling(scaling);
        }

        /* Populate the combined transformation and object mapping only if
           there's actually some transformation for this object and we want to
           store it -- if all objects have TRS anyway, the matrix is redundant */
        if((node.matrix.size() != 0 ||
            node.translation.size() != 0 ||
            node.rotation.size() != 0 ||
            node.scale.size() != 0) && transformationCount)
        {
            transformations[transformationOffset] = transformation;
            transformationObjects[transformationOffset] = objects[i];
            ++transformationOffset;
        }

        /* Store the TRS information and object mapping only if there was
           something */
        if(node.translation.size() != 0 ||
           node.rotation.size() != 0 ||
           node.scale.size() != 0)
        {
            if(hasTranslations) translations[trsOffset] = translation;
            if(hasRotations) rotations[trsOffset] = rotation;
            if(hasScalings) scalings[trsOffset] = scaling;
            trsObjects[trsOffset] = objects[i];
            ++trsOffset;
        }

        /* Populate mesh references */
        if(node.mesh != -1) {
            /* Bounds-checked in the counting loop above already */

            const tinygltf::Mesh& mesh = _d->model.meshes[node.mesh];
            for(std::size_t j = 0; j != mesh.primitives.size(); ++j) {
                meshMaterialObjects[meshMaterialOffset] = objects[i];
                meshes[meshMaterialOffset] = _d->meshSizeOffsets[node.mesh] + j;
                if(hasMeshMaterials) {
                    const Int material = mesh.primitives[j].material;
                    if(material != -1 && UnsignedInt(material) >= _d->model.materials.size()) {
                        Error{} << "Trade::TinyGltfImporter::scene(): material index" << material << "in node" << objects[i] << "out of range for" << _d->model.materials.size() << "materials";
                        return {};
                    }
                    meshMaterials[meshMaterialOffset] = mesh.primitives[j].material;
                }
                ++meshMaterialOffset;
            }
        }

        /* Populate light references */
        if(node.extensions.find("KHR_lights_punctual") != node.extensions.end()) {
            const UnsignedInt light = node.extensions.at("KHR_lights_punctual").Get("light").Get<int>();

            if(light >= _d->model.lights.size()) {
                Error{} << "Trade::TinyGltfImporter::scene(): light index" << light << "in node" << objects[i] << "out of range for" << _d->model.lights.size() << "lights";
                return {};
            }

            lightObjects[lightOffset] = objects[i];
            lights[lightOffset] = light;
            ++lightOffset;
        }

        /* Populate camera references */
        if(node.camera != -1) {
            if(UnsignedInt(node.camera) >= _d->model.cameras.size()) {
                Error{} << "Trade::TinyGltfImporter::scene(): camera index" << node.camera << "in node" << objects[i] << "out of range for" << _d->model.cameras.size() << "cameras";
                return {};
            }

            cameraObjects[cameraOffset] = objects[i];
            cameras[cameraOffset] = node.camera;
            ++cameraOffset;
        }

        /* Populate skin references */
        if(node.skin != -1) {
            if(UnsignedInt(node.skin) >= _d->model.skins.size()) {
                Error{} << "Trade::TinyGltfImporter::scene(): skin index" << node.skin << "in node" << objects[i] << "out of range for" << _d->model.skins.size() << "skins";
                return {};
            }

            skinObjects[skinOffset] = objects[i];
            skins[skinOffset] = node.skin;
            ++skinOffset;
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
        SceneFieldData{SceneField::ImporterState, parentImporterStateObjects, importerState},
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

    if(meshCount) arrayAppend(fields, SceneFieldData{
        SceneField::Mesh, meshMaterialObjects, meshes
    });
    if(hasMeshMaterials) arrayAppend(fields, SceneFieldData{
        SceneField::MeshMaterial, meshMaterialObjects, meshMaterials
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

    /* Convert back to the default deleter to avoid dangling deleter function
       pointer issues when unloading the plugin */
    arrayShrink(fields, ValueInit);
    return SceneData{SceneMappingType::UnsignedInt, maxObjectIndex + 1, std::move(data), std::move(fields), &scene};
}

UnsignedLong TinyGltfImporter::doObjectCount() const {
    return _d->model.nodes.size();
}

Long TinyGltfImporter::doObjectForName(const Containers::StringView name) {
    if(!_d->nodesForName) {
        _d->nodesForName.emplace();
        _d->nodesForName->reserve(_d->model.nodes.size());
        for(std::size_t i = 0; i != _d->model.nodes.size(); ++i) {
            _d->nodesForName->emplace(_d->model.nodes[i].name, i);
        }
    }

    const auto found = _d->nodesForName->find(name);
    return found == _d->nodesForName->end() ? -1 : found->second;
}

Containers::String TinyGltfImporter::doObjectName(UnsignedLong id) {
    return _d->model.nodes[id].name;
}

UnsignedInt TinyGltfImporter::doSkin3DCount() const {
    return _d->model.skins.size();
}

Int TinyGltfImporter::doSkin3DForName(const Containers::StringView name) {
    if(!_d->skinsForName) {
        _d->skinsForName.emplace();
        _d->skinsForName->reserve(_d->model.skins.size());
        for(std::size_t i = 0; i != _d->model.skins.size(); ++i)
            _d->skinsForName->emplace(_d->model.skins[i].name, i);
    }

    const auto found = _d->skinsForName->find(name);
    return found == _d->skinsForName->end() ? -1 : found->second;
}

Containers::String TinyGltfImporter::doSkin3DName(const UnsignedInt id) {
    return _d->model.skins[id].name;
}

Containers::Optional<SkinData3D> TinyGltfImporter::doSkin3D(const UnsignedInt id) {
    const tinygltf::Skin& skin = _d->model.skins[id];

    if(skin.joints.empty()) {
        Error{} << "Trade::TinyGltfImporter::skin3D(): skin has no joints";
        return Containers::NullOpt;
    }

    /* Joint IDs */
    Containers::Array<UnsignedInt> joints{NoInit, skin.joints.size()};
    for(std::size_t i = 0; i != joints.size(); ++i) {
        if(std::size_t(skin.joints[i]) >= _d->model.nodes.size()) {
            Error{} << "Trade::TinyGltfImporter::skin3D(): target node" << skin.joints[i] << "out of range for" << _d->model.nodes.size() << "nodes";
            return Containers::NullOpt;
        }

        joints[i] = skin.joints[i];
    }

    /* Inverse bind matrices. If there are none, default is identities */
    Containers::Array<Matrix4> inverseBindMatrices{skin.joints.size()};
    if(skin.inverseBindMatrices != -1) {
        const tinygltf::Accessor* accessor = checkedAccessor(_d->model, "skin3D", skin.inverseBindMatrices);
        if(!accessor) return Containers::NullOpt;

        if(accessor->type != TINYGLTF_TYPE_MAT4 || accessor->componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
            Error{} << "Trade::TinyGltfImporter::skin3D(): inverse bind matrices have unexpected type" << accessor->type << Debug::nospace << "/" << Debug::nospace << accessor->componentType;
            return Containers::NullOpt;
        }

        Containers::StridedArrayView1D<const Matrix4> data = Containers::arrayCast<1, const Matrix4>(bufferView(_d->model, *accessor));
        if(data.size() != inverseBindMatrices.size()) {
            Error{} << "Trade::TinyGltfImporter::skin3D(): invalid inverse bind matrix count, expected" << inverseBindMatrices.size() << "but got" << data.size();
            return Containers::NullOpt;
        }

        Utility::copy(data, inverseBindMatrices);
    }

    return SkinData3D{std::move(joints), std::move(inverseBindMatrices), &skin};
}

UnsignedInt TinyGltfImporter::doMeshCount() const {
    return _d->meshMap.size();
}

Int TinyGltfImporter::doMeshForName(const Containers::StringView name) {
    if(!_d->meshesForName) {
        _d->meshesForName.emplace();
        _d->meshesForName->reserve(_d->model.meshes.size());
        for(std::size_t i = 0; i != _d->model.meshes.size(); ++i) {
            /* The mesh can be duplicated for as many primitives as it has,
               point to the first mesh in the duplicate sequence */
            _d->meshesForName->emplace(_d->model.meshes[i].name, _d->meshSizeOffsets[i]);
        }
    }

    const auto found = _d->meshesForName->find(name);
    return found == _d->meshesForName->end() ? -1 : found->second;
}

Containers::String TinyGltfImporter::doMeshName(const UnsignedInt id) {
    /* This returns the same name for all multi-primitive mesh duplicates */
    return _d->model.meshes[_d->meshMap[id].first].name;
}

Containers::Optional<MeshData> TinyGltfImporter::doMesh(const UnsignedInt id, UnsignedInt) {
    const tinygltf::Mesh& mesh = _d->model.meshes[_d->meshMap[id].first];
    const tinygltf::Primitive& primitive = mesh.primitives[_d->meshMap[id].second];

    MeshPrimitive meshPrimitive{};
    if(primitive.mode == TINYGLTF_MODE_POINTS) {
        meshPrimitive = MeshPrimitive::Points;
    } else if(primitive.mode == TINYGLTF_MODE_LINE) {
        meshPrimitive = MeshPrimitive::Lines;
    } else if(primitive.mode == TINYGLTF_MODE_LINE_LOOP) {
        meshPrimitive = MeshPrimitive::LineLoop;
    } else if(primitive.mode == TINYGLTF_MODE_LINE_STRIP) {
        meshPrimitive = MeshPrimitive::LineStrip;
    } else if(primitive.mode == TINYGLTF_MODE_TRIANGLES) {
        meshPrimitive = MeshPrimitive::Triangles;
    } else if(primitive.mode == TINYGLTF_MODE_TRIANGLE_FAN) {
        meshPrimitive = MeshPrimitive::TriangleFan;
    } else if(primitive.mode == TINYGLTF_MODE_TRIANGLE_STRIP) {
        meshPrimitive = MeshPrimitive::TriangleStrip;
    } else {
        Error{} << "Trade::TinyGltfImporter::mesh(): unrecognized primitive" << primitive.mode;
        return Containers::NullOpt;
    }

    /* Gather all (whitelisted) attributes and the total buffer range spanning
       them */
    UnsignedInt vertexCount = 0;
    std::size_t attributeId = 0;
    /* This gets always filled for the first attribute, however GCC 11 fails to
       see that and fires a -Wmaybe-uninitialized warning if I don't init. */
    std::size_t bufferId = ~std::size_t{};
    Containers::StringView lastAttributeSemantic;
    Int lastAttributeIndex = -1;
    Math::Range1D<std::size_t> bufferRange;
    Containers::Array<MeshAttributeData> attributeData{primitive.attributes.size()};
    for(auto& attribute: primitive.attributes) {
        auto* acessorPointer = checkedAccessor(_d->model, "mesh", attribute.second);
        if(!acessorPointer) return Containers::NullOpt;
        const tinygltf::Accessor& accessor = *acessorPointer;

        const Containers::StringView semantic = attributeSemantic(attribute.first);

        /* Numbered attributes are expected to be contiguous (COLORS_0,
           COLORS_1...). If not, print a warning, because in the MeshData they
           will appear as contiguous. */
        if(!semantic.isEmpty()) {
            if(semantic != lastAttributeSemantic)
                lastAttributeIndex = -1;

            const Int index = std::strtol(attribute.first.c_str() + semantic.size() + 1, nullptr, 10);
            if(index != lastAttributeIndex + 1)
                Warning{} << "Trade::TinyGltfImporter::mesh(): found attribute" << attribute.first << "but expected" << semantic << Debug::nospace << "_" << Debug::nospace << lastAttributeIndex + 1;

            lastAttributeSemantic = semantic;
            lastAttributeIndex = index;
        }

        /* Whitelist supported name and type combinations */
        MeshAttribute name;
        if(attribute.first == "POSITION") {
            name = MeshAttribute::Position;

            if(accessor.type != TINYGLTF_TYPE_VEC3) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unexpected POSITION type" << accessor.type;
                return Containers::NullOpt;
            }

            if(!(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !accessor.normalized) &&
               /* Both normalized and unnormalized bytes/shorts are okay */
               accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
               accessor.componentType != TINYGLTF_COMPONENT_TYPE_BYTE &&
               accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
               accessor.componentType != TINYGLTF_COMPONENT_TYPE_SHORT) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unsupported POSITION component type"
                    << (accessor.normalized ? "normalized" : "unnormalized")
                    << accessor.componentType;
                return Containers::NullOpt;
            }

        } else if(attribute.first == "NORMAL") {
            name = MeshAttribute::Normal;

            if(accessor.type != TINYGLTF_TYPE_VEC3) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unexpected NORMAL type" << accessor.type;
                return Containers::NullOpt;
            }

            if(!(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !accessor.normalized) &&
               !(accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE && accessor.normalized) &&
               !(accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT && accessor.normalized)) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unsupported NORMAL component type"
                    << (accessor.normalized ? "normalized" : "unnormalized")
                    << accessor.componentType;
                return Containers::NullOpt;
            }

        } else if(attribute.first == "TANGENT") {
            name = MeshAttribute::Tangent;

            if(accessor.type != TINYGLTF_TYPE_VEC4) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unexpected TANGENT type" << accessor.type;
                return Containers::NullOpt;
            }

            if(!(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !accessor.normalized) &&
               !(accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE && accessor.normalized) &&
               !(accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT && accessor.normalized)) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unsupported TANGENT component type"
                    << (accessor.normalized ? "normalized" : "unnormalized")
                    << accessor.componentType;
                return Containers::NullOpt;
            }

        /* Texture coordinate attribute ends with _0, _1 ... */
        } else if(semantic == "TEXCOORD") {
            name = MeshAttribute::TextureCoordinates;

            if(accessor.type != TINYGLTF_TYPE_VEC2) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unexpected TEXCOORD type" << accessor.type;
                return Containers::NullOpt;
            }

            if(!(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !accessor.normalized) &&
               /* Both normalized and unnormalized bytes/shorts are okay */
               accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
               accessor.componentType != TINYGLTF_COMPONENT_TYPE_BYTE &&
               accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
               accessor.componentType != TINYGLTF_COMPONENT_TYPE_SHORT) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unsupported TEXCOORD component type"
                    << (accessor.normalized ? "normalized" : "unnormalized")
                    << accessor.componentType;
                return Containers::NullOpt;
            }

        /* Color attribute ends with _0, _1 ... */
        } else if(semantic == "COLOR") {
            name = MeshAttribute::Color;

            if(accessor.type != TINYGLTF_TYPE_VEC4 && accessor.type != TINYGLTF_TYPE_VEC3) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unexpected COLOR type" << accessor.type;
                return Containers::NullOpt;
            }

            if(!(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !accessor.normalized) &&
               !(accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE && accessor.normalized) &&
               !(accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT && accessor.normalized)) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unsupported COLOR component type"
                    << (accessor.normalized ? "normalized" : "unnormalized")
                    << accessor.componentType;
                return Containers::NullOpt;
            }

        /* Joint IDs attribute ends with _0, _1 ... */
        } else if(semantic == "JOINTS") {
            name = _d->meshAttributesForName.at(attribute.first);

            if(accessor.type != TINYGLTF_TYPE_VEC4) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unexpected JOINTS type" << accessor.type;
                return Containers::NullOpt;
            }

            if(!(accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE && !accessor.normalized) &&
               !(accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT && !accessor.normalized)) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unsupported JOINTS component type"
                    << (accessor.normalized ? "normalized" : "unnormalized")
                    << accessor.componentType;
                return Containers::NullOpt;
            }

        /* Joint weights attribute ends with _0, _1 ... */
        } else if(semantic == "WEIGHTS") {
            name = _d->meshAttributesForName.at(attribute.first);

            if(accessor.type != TINYGLTF_TYPE_VEC4) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unexpected WEIGHTS type" << accessor.type;
                return Containers::NullOpt;
            }

            if(!(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !accessor.normalized) &&
               !(accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE && accessor.normalized) &&
               !(accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT && accessor.normalized)) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unsupported WEIGHTS component type"
                    << (accessor.normalized ? "normalized" : "unnormalized")
                    << accessor.componentType;
                return Containers::NullOpt;
            }

        /* Object ID, name user-configurable */
        } else if(attribute.first == configuration().value("objectIdAttribute")) {
            name = MeshAttribute::ObjectId;

            if(accessor.type != TINYGLTF_TYPE_SCALAR) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unexpected object ID type" << accessor.type;
                return Containers::NullOpt;
            }

            if((accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT &&
                accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
                accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) ||
                accessor.normalized) {
                Error{} << "Trade::TinyGltfImporter::mesh(): unsupported object ID component type"
                    << (accessor.normalized ? "normalized" : "unnormalized")
                    << accessor.componentType;
                return Containers::NullOpt;
            }

        /* Custom or unrecognized attributes, map to an ID */
        } else name = _d->meshAttributesForName.at(attribute.first);

        /* Convert to our vertex format */
        VertexFormat componentFormat;
        if(accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE)
            componentFormat = VertexFormat::Byte;
        else if(accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
            componentFormat = VertexFormat::UnsignedByte;
        else if(accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT)
            componentFormat = VertexFormat::Short;
        else if(accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
            componentFormat = VertexFormat::UnsignedShort;
        else if(accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
            componentFormat = VertexFormat::UnsignedInt;
        else if(accessor.componentType == TINYGLTF_COMPONENT_TYPE_INT)
            componentFormat = VertexFormat::Int;
        else if(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
            componentFormat = VertexFormat::Float;
        else if(accessor.componentType == TINYGLTF_COMPONENT_TYPE_DOUBLE)
            componentFormat = VertexFormat::Double;
        else CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */

        UnsignedInt componentCount;
        UnsignedInt vectorCount = 0;
        if(accessor.type == TINYGLTF_TYPE_SCALAR)
            componentCount = 1;
        else if(accessor.type == TINYGLTF_TYPE_VEC2)
            componentCount = 2;
        else if(accessor.type == TINYGLTF_TYPE_VEC3)
            componentCount = 3;
        else if(accessor.type == TINYGLTF_TYPE_VEC4)
            componentCount = 4;
        else if(accessor.type == TINYGLTF_TYPE_MAT2) {
            componentCount = 2;
            vectorCount = 2;
        } else if(accessor.type == TINYGLTF_TYPE_MAT3) {
            componentCount = 3;
            vectorCount = 3;
        } else if(accessor.type == TINYGLTF_TYPE_MAT4) {
            componentCount = 4;
            vectorCount = 4;
        } else CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */

        /* Check for illegal normalized types */
        if(accessor.normalized &&
           componentFormat != VertexFormat::Byte &&
           componentFormat != VertexFormat::UnsignedByte &&
           componentFormat != VertexFormat::Short &&
           componentFormat != VertexFormat::UnsignedShort) {
            Error{} << "Trade::TinyGltfImporter::mesh(): component type" << accessor.componentType << "can't be normalized";
            return Containers::NullOpt;
        }

        /* Check that matrix type is legal */
        if(vectorCount &&
            componentFormat != VertexFormat::Float &&
            componentFormat != VertexFormat::Double &&
            !(componentFormat == VertexFormat::Byte && accessor.normalized) &&
            !(componentFormat == VertexFormat::Short && accessor.normalized)) {
            Error{} << "Trade::TinyGltfImporter::mesh(): unsupported matrix component type"
                << (accessor.normalized ? "normalized" : "unnormalized")
                << accessor.componentType;
            return Containers::NullOpt;
        }

        const VertexFormat format = vectorCount ?
            vertexFormat(componentFormat, vectorCount, componentCount, true) :
            vertexFormat(componentFormat, componentCount, accessor.normalized);

        /* Remember which buffer the attribute is in and the range, for
           consecutive attribs expand the range */
        const tinygltf::BufferView& bufferView = _d->model.bufferViews[accessor.bufferView];
        if(attributeId == 0) {
            bufferId = bufferView.buffer;
            bufferRange = Math::Range1D<std::size_t>::fromSize(bufferView.byteOffset, bufferView.byteLength);
            vertexCount = accessor.count;
        } else {
            /* ... and probably never will be */
            if(std::size_t(bufferView.buffer) != bufferId) {
                Error{} << "Trade::TinyGltfImporter::mesh(): meshes spanning multiple buffers are not supported";
                return Containers::NullOpt;
            }

            bufferRange = Math::join(bufferRange, Math::Range1D<std::size_t>::fromSize(bufferView.byteOffset, bufferView.byteLength));

            if(accessor.count != vertexCount) {
                Error{} << "Trade::TinyGltfImporter::mesh(): mismatched vertex count for attribute" << attribute.first << Debug::nospace << ", expected" << vertexCount << "but got" << accessor.count;
                return Containers::NullOpt;
            }
        }

        /* Fill in an attribute. Offset-only, will be patched to be relative to
           the actual output buffer once we know how large it is and where it
           is allocated. */
        attributeData[attributeId++] = MeshAttributeData{name, format,
            UnsignedInt(accessor.byteOffset + bufferView.byteOffset), vertexCount,
            /* Stride could be 0, in which case it's equal to element size */
            std::ptrdiff_t(bufferView.byteStride ? bufferView.byteStride : vertexFormatSize(format))};
    }

    /* Verify we really filled all attributes */
    CORRADE_INTERNAL_ASSERT(attributeId == attributeData.size());

    /* Allocate & copy vertex data (if any) */
    Containers::Array<char> vertexData{NoInit, bufferRange.size()};
    if(vertexData.size()) Utility::copy(Containers::arrayCast<const char>(
        Containers::arrayView(_d->model.buffers[bufferId].data)
            .slice(bufferRange.min(), bufferRange.max())),
        vertexData);

    /* Convert the attributes from relative to absolute, copy them to a
       non-growable array and do additional patching */
    for(std::size_t i = 0; i != attributeData.size(); ++i) {
        Containers::StridedArrayView1D<char> data{vertexData,
            /* Offset is what with the range min subtracted, as we copied
               without the prefix */
            vertexData + attributeData[i].offset(vertexData) - bufferRange.min(),
            vertexCount, attributeData[i].stride()};

        attributeData[i] = MeshAttributeData{attributeData[i].name(),
            attributeData[i].format(), data};

        /* Flip Y axis of texture coordinates, unless it's done in the material
           instead */
        if(attributeData[i].name() == MeshAttribute::TextureCoordinates && !_d->textureCoordinateYFlipInMaterial) {
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
    }

    /* Indices */
    MeshIndexData indices;
    Containers::Array<char> indexData;
    if(primitive.indices != -1) {
        const tinygltf::Accessor* accessor = checkedAccessor(_d->model, "mesh", primitive.indices);
        if(!accessor) return Containers::NullOpt;

        if(accessor->type != TINYGLTF_TYPE_SCALAR) {
            Error() << "Trade::TinyGltfImporter::mesh(): unexpected index type" << accessor->type;
            return Containers::NullOpt;
        }

        if(accessor->normalized) {
            Error() << "Trade::TinyGltfImporter::mesh(): index type can't be normalized";
            return Containers::NullOpt;
        }

        MeshIndexType type;
        if(accessor->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
            type = MeshIndexType::UnsignedByte;
        else if(accessor->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
            type = MeshIndexType::UnsignedShort;
        else if(accessor->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
            type = MeshIndexType::UnsignedInt;
        else {
            Error{} << "Trade::TinyGltfImporter::mesh(): unexpected index component type" << accessor->componentType;
            return Containers::NullOpt;
        }

        Containers::StridedArrayView2D<const char> src = bufferView(_d->model, *accessor);
        if(!src.isContiguous()) {
            Error{} << "Trade::TinyGltfImporter::mesh(): index bufferView is not contiguous";
            return Containers::NullOpt;
        }

        Containers::ArrayView<const char> srcContiguous = src.asContiguous();
        indexData = Containers::Array<char>{srcContiguous.size()};
        Utility::copy(srcContiguous, indexData);
        indices = MeshIndexData{type, indexData};
    }

    /* If we have an index-less attribute-less mesh, glTF has no way to supply
       a vertex count, so return 0 */
    if(!indices.data().size() && !attributeData.size())
        return MeshData{meshPrimitive, 0, &mesh};

    return MeshData{meshPrimitive,
        std::move(indexData), indices,
        std::move(vertexData), std::move(attributeData),
        vertexCount, &mesh};
}

MeshAttribute TinyGltfImporter::doMeshAttributeForName(const Containers::StringView name) {
    return _d ? _d->meshAttributesForName[name] : MeshAttribute{};
}

Containers::String TinyGltfImporter::doMeshAttributeName(MeshAttribute name) {
    return _d && meshAttributeCustom(name) < _d->meshAttributeNames.size() ?
        _d->meshAttributeNames[meshAttributeCustom(name)] : "";
}

UnsignedInt TinyGltfImporter::doMaterialCount() const {
    return _d->model.materials.size();
}

Int TinyGltfImporter::doMaterialForName(const Containers::StringView name) {
    if(!_d->materialsForName) {
        _d->materialsForName.emplace();
        _d->materialsForName->reserve(_d->model.materials.size());
        for(std::size_t i = 0; i != _d->model.materials.size(); ++i)
            _d->materialsForName->emplace(_d->model.materials[i].name, i);
    }

    const auto found = _d->materialsForName->find(name);
    return found == _d->materialsForName->end() ? -1 : found->second;
}

Containers::String TinyGltfImporter::doMaterialName(const UnsignedInt id) {
    return _d->model.materials[id].name;
}

bool TinyGltfImporter::materialTexture(const char* name, const UnsignedInt texture, UnsignedInt texCoord, const tinygltf::Value& extensions, Containers::Array<MaterialAttributeData>& attributes, const MaterialAttribute attribute, const MaterialAttribute matrixAttribute, const MaterialAttribute coordinateAttribute) const {
    if(texture >= _d->model.textures.size()) {
        Error{} << "Trade::TinyGltfImporter::material():" << name << "index" << texture << "out of range for" << _d->model.textures.size() << "textures";
        return false;
    }

    /* Texture transform. Because texture coordinates were Y-flipped, we first
       unflip them back, apply the transform (which assumes origin at bottom
       left and Y down) and then flip the result again. Sanity of the following
       verified with https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/TextureTransformTest */
    bool hasTextureTransform = false;
    if(extensions.Type() == tinygltf::OBJECT_TYPE) {
        auto khrTextureTransform = extensions.Get("KHR_texture_transform");
        if(khrTextureTransform.Type() != tinygltf::NULL_TYPE) {
            hasTextureTransform = true;
            Matrix3 matrix;

            /* If material needs an Y-flip, the mesh doesn't have the texture
               coordinates flipped and thus we don't need to unflip them first */
            if(!_d->textureCoordinateYFlipInMaterial)
                matrix = Matrix3::translation(Vector2::yAxis(1.0f))*
                         Matrix3::scaling(Vector2::yScale(-1.0f));

            /* The extension can override texture coordinate index (for example
               to have the unextended coordinates already transformed, and
               applying transformation to a different set) */
            auto transformTexCoord = khrTextureTransform.Get("texCoord");
            if(transformTexCoord.Type() != tinygltf::NULL_TYPE)
                texCoord = transformTexCoord.Get<int>();

            auto scale = khrTextureTransform.Get("scale");
            if(scale.Type() != tinygltf::NULL_TYPE) {
                matrix = Matrix3::scaling(Vector2{Vector2d{
                        scale.Get(0).Get<double>(),
                        scale.Get(1).Get<double>()}}
                    )*matrix;
            }

            /* Because we import images with Y flipped, counterclockwise
               rotation is now clockwise. This has to be done in addition
               to the Y flip/unflip. */
            auto rotation = khrTextureTransform.Get("rotation");
            if(rotation.Type() != tinygltf::NULL_TYPE) {
                matrix = Matrix3::rotation(
                        -Rad(Radd(rotation.Get<double>()))
                    )*matrix;
            }

            auto offset = khrTextureTransform.Get("offset");
            if(offset.Type() != tinygltf::NULL_TYPE) {
                matrix = Matrix3::translation(Vector2{Vector2d{
                        offset.Get(0).Get<double>(),
                        offset.Get(1).Get<double>()}}
                    )*matrix;
            }

            matrix = Matrix3::translation(Vector2::yAxis(1.0f))*
                     Matrix3::scaling(Vector2::yScale(-1.0f))*matrix;

            arrayAppend(attributes, InPlaceInit, matrixAttribute, matrix);
        }
    }

    /* In case the material had no texture transformation but still needs an
       Y-flip, put it there */
    if(!hasTextureTransform && _d->textureCoordinateYFlipInMaterial) {
        arrayAppend(attributes, InPlaceInit, matrixAttribute,
            Matrix3::translation(Vector2::yAxis(1.0f))*
            Matrix3::scaling(Vector2::yScale(-1.0f)));
    }

    /* Add texture coordinate set if non-zero. The KHR_texture_transform
       could be modifying it, so do that after */
    if(texCoord != 0)
        arrayAppend(attributes, InPlaceInit, coordinateAttribute, texCoord);

    /* In some cases (when dealing with packed textures), we're parsing &
       adding texture coordinates and matrix multiple times, but adding the
       packed texture ID just once. In other cases the attribute is invalid. */
    if(attribute != MaterialAttribute{})
        arrayAppend(attributes, InPlaceInit, attribute, texture);

    return true;
}

Containers::Optional<MaterialData> TinyGltfImporter::doMaterial(const UnsignedInt id) {
    const tinygltf::Material& material = _d->model.materials[id];

    Containers::Array<UnsignedInt> layers;
    Containers::Array<MaterialAttributeData> attributes;
    MaterialTypes types;

    /* Alpha mode and mask, double sided */
    if(material.alphaMode == "BLEND")
        arrayAppend(attributes, InPlaceInit, MaterialAttribute::AlphaBlend, true);
    else if(material.alphaMode == "MASK")
        arrayAppend(attributes, InPlaceInit, MaterialAttribute::AlphaMask, Float(material.alphaCutoff));
    else if(material.alphaMode != "OPAQUE") {
        Error{} << "Trade::TinyGltfImporter::material(): unknown alpha mode" << material.alphaMode;
        return Containers::NullOpt;
    }

    if(material.doubleSided)
        arrayAppend(attributes, InPlaceInit, MaterialAttribute::DoubleSided, true);

    /* Core metallic/roughness material */
    /** @todo is there ANY way to check if these properties are actually
        present?! tinygltf FFS */
    {
        types |= MaterialType::PbrMetallicRoughness;

        if(Vector4d::from(material.pbrMetallicRoughness.baseColorFactor.data()) != Vector4d{1.0})
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::BaseColor,
                Color4{Vector4d::from(material.pbrMetallicRoughness.baseColorFactor.data())});
        if(material.pbrMetallicRoughness.metallicFactor != 1.0)
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::Metalness,
                Float(material.pbrMetallicRoughness.metallicFactor));
        if(material.pbrMetallicRoughness.roughnessFactor != 1.0)
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::Roughness,
                Float(material.pbrMetallicRoughness.roughnessFactor));

        const Int baseColorTexture = material.pbrMetallicRoughness.baseColorTexture.index;
        if(baseColorTexture != -1) {
            if(!materialTexture("baseColorTexture", baseColorTexture,
                material.pbrMetallicRoughness.baseColorTexture.texCoord,
                /* YES, you guessed right, this does a deep copy of the nested
                   std::maps because tinygltf is SO GREAT that there's NO WAY
                   to access extension structures in a consistent way */
                tinygltf::Value(material.pbrMetallicRoughness.baseColorTexture.extensions),
                attributes,
                MaterialAttribute::BaseColorTexture,
                MaterialAttribute::BaseColorTextureMatrix,
                MaterialAttribute::BaseColorTextureCoordinates)
            )
                return Containers::NullOpt;
        }

        const Int metallicRoughnessTexture = material.pbrMetallicRoughness.metallicRoughnessTexture.index;
        if(metallicRoughnessTexture != -1) {
            if(!materialTexture("metallicRoughnessTexture",
                metallicRoughnessTexture,
                material.pbrMetallicRoughness.metallicRoughnessTexture.texCoord,
                /* YES, you guessed right, this does a deep copy of the nested
                   std::maps because tinygltf is SO GREAT that there's NO WAY
                   to access extension structures in a consistent way */
                tinygltf::Value(material.pbrMetallicRoughness.metallicRoughnessTexture.extensions),
                attributes,
                MaterialAttribute::NoneRoughnessMetallicTexture,
                MaterialAttribute::MetalnessTextureMatrix,
                MaterialAttribute::MetalnessTextureCoordinates)
            )
                return Containers::NullOpt;

            /* Add the matrix/coordinates attributes also for the roughness
               texture, but skip adding the texture ID again */
            CORRADE_INTERNAL_ASSERT_OUTPUT(materialTexture("metallicRoughnessTexture",
                metallicRoughnessTexture,
                material.pbrMetallicRoughness.metallicRoughnessTexture.texCoord,
                /* YES, you guessed right, this does a deep copy of the nested
                   std::maps because tinygltf is SO GREAT that there's NO WAY
                   to access extension structures in a consistent way */
                tinygltf::Value(material.pbrMetallicRoughness.metallicRoughnessTexture.extensions),
                attributes,
                MaterialAttribute{},
                MaterialAttribute::RoughnessTextureMatrix,
                MaterialAttribute::RoughnessTextureCoordinates));
        }
    }

    /* Specular/glossiness material */
    auto khrMaterialsPbrSpecularGlossiness = material.extensions.find("KHR_materials_pbrSpecularGlossiness");
    if(khrMaterialsPbrSpecularGlossiness != material.extensions.end()) {
        types |= MaterialType::PbrSpecularGlossiness;

        auto diffuseColor = khrMaterialsPbrSpecularGlossiness->second.Get("diffuseFactor");
        if(diffuseColor.Type() != tinygltf::NULL_TYPE) {
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::DiffuseColor, Vector4{Vector4d{
                diffuseColor.Get(0).Get<double>(),
                diffuseColor.Get(1).Get<double>(),
                diffuseColor.Get(2).Get<double>(),
                diffuseColor.Get(3).Get<double>()}});
        }

        auto specularColor = khrMaterialsPbrSpecularGlossiness->second.Get("specularFactor");
        if(specularColor.Type() != tinygltf::NULL_TYPE) {
            arrayAppend(attributes, InPlaceInit,
                /* Specular is 3-component in glTF, alpha should be 0 to not
                   affect transparent materials */
                MaterialAttribute::SpecularColor, Color4{Vector3{Vector3d{
                specularColor.Get(0).Get<double>(),
                specularColor.Get(1).Get<double>(),
                specularColor.Get(2).Get<double>()}}, 0.0f});
        }

        auto glossiness = khrMaterialsPbrSpecularGlossiness->second.Get("glossinessFactor");
        if(glossiness.Type() != tinygltf::NULL_TYPE) {
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::Glossiness,
                Float(glossiness.Get<double>()));
        }

        auto diffuseTexture = khrMaterialsPbrSpecularGlossiness->second.Get("diffuseTexture");
        if(diffuseTexture.Type() != tinygltf::NULL_TYPE) {
            if(!materialTexture("diffuseTexture",
                diffuseTexture.Get("index").Get<int>(),
                diffuseTexture.Get("texCoord").Get<int>(),
                diffuseTexture.Get("extensions"),
                attributes,
                MaterialAttribute::DiffuseTexture,
                MaterialAttribute::DiffuseTextureMatrix,
                MaterialAttribute::DiffuseTextureCoordinates)
            )
                return Containers::NullOpt;
        }

        auto specularGlossinessTexture = khrMaterialsPbrSpecularGlossiness->second.Get("specularGlossinessTexture");
        if(specularGlossinessTexture.Type() != tinygltf::NULL_TYPE) {
            if(!materialTexture("specularGlossinessTexture",
                specularGlossinessTexture.Get("index").Get<int>(),
                specularGlossinessTexture.Get("texCoord").Get<int>(),
                specularGlossinessTexture.Get("extensions"),
                attributes,
                MaterialAttribute::SpecularGlossinessTexture,
                MaterialAttribute::SpecularTextureMatrix,
                MaterialAttribute::SpecularTextureCoordinates)
            )
                return Containers::NullOpt;

            /* Add the matrix/coordinates attributes also for the glossiness
               texture, but skip adding the texture ID again */
            CORRADE_INTERNAL_ASSERT_OUTPUT(materialTexture(
                "specularGlossinessTexture",
                specularGlossinessTexture.Get("index").Get<int>(),
                specularGlossinessTexture.Get("texCoord").Get<int>(),
                specularGlossinessTexture.Get("extensions"),
                attributes,
                MaterialAttribute{},
                MaterialAttribute::GlossinessTextureMatrix,
                MaterialAttribute::GlossinessTextureCoordinates));
        }
    }

    /* Unlit material -- reset all types and add just Flat */
    auto khrMaterialsUnlit = material.extensions.find("KHR_materials_unlit");
    if(khrMaterialsUnlit != material.extensions.end()) {
        types = MaterialType::Flat;
    }

    /* Normal texture */
    const Int normalTexture = material.normalTexture.index;
    if(normalTexture != -1) {
        if(!materialTexture("normalTexture", normalTexture,
            material.normalTexture.texCoord,
            /* YES, you guessed right, this does a deep copy of the nested
               std::maps because tinygltf is SO GREAT that there's NO WAY
               to access extension structures in a consistent way */
            tinygltf::Value(material.normalTexture.extensions),
            attributes,
            MaterialAttribute::NormalTexture,
            MaterialAttribute::NormalTextureMatrix,
            MaterialAttribute::NormalTextureCoordinates)
        )
            return Containers::NullOpt;

        if(material.normalTexture.scale != 1.0)
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::NormalTextureScale,
                Float(material.normalTexture.scale));
    }

    /* Occlusion texture */
    const Int occlusionTexture = material.occlusionTexture.index;
    if(occlusionTexture != -1) {
        if(!materialTexture("occlusionTexture", occlusionTexture,
            material.occlusionTexture.texCoord,
            /* YES, you guessed right, this does a deep copy of the nested
               std::maps because tinygltf is SO GREAT that there's NO WAY
               to access extension structures in a consistent way */
            tinygltf::Value(material.occlusionTexture.extensions),
            attributes,
            MaterialAttribute::OcclusionTexture,
            MaterialAttribute::OcclusionTextureMatrix,
            MaterialAttribute::OcclusionTextureCoordinates)
        )
            return Containers::NullOpt;

        if(material.occlusionTexture.strength != 1.0)
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::OcclusionTextureStrength,
                Float(material.occlusionTexture.strength));
    }

    /* Emissive factor & texture */
    if(Vector3d::from(material.emissiveFactor.data()) != Vector3d{0.0})
        arrayAppend(attributes, InPlaceInit,
            MaterialAttribute::EmissiveColor,
            Color3{Vector3d::from(material.emissiveFactor.data())});
    const Int emissiveTexture = material.emissiveTexture.index;
    if(emissiveTexture != -1) {
        if(!materialTexture("emissiveTexture", emissiveTexture,
            material.emissiveTexture.texCoord,
            /* YES, you guessed right, this does a deep copy of the nested
               std::maps because tinygltf is SO GREAT that there's NO WAY
               to access extension structures in a consistent way */
            tinygltf::Value(material.emissiveTexture.extensions),
            attributes,
            MaterialAttribute::EmissiveTexture,
            MaterialAttribute::EmissiveTextureMatrix,
            MaterialAttribute::EmissiveTextureCoordinates)
        )
            return Containers::NullOpt;
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
        for(const MaterialAttributeData& attribute: attributes) {
            if(attribute.name() == "BaseColor")
                diffuseColor = attribute.value<Color4>();
            else if(attribute.name() == "BaseColorTexture")
                diffuseTexture = attribute.value<UnsignedInt>();
            else if(attribute.name() == "BaseColorTextureMatrix")
                diffuseTextureMatrix = attribute.value<Matrix3>();
            else if(attribute.name() == "BaseColorTextureCoordinates")
                diffuseTextureCoordinates = attribute.value<UnsignedInt>();
        }

        /* But if there already are those from the specular/glossiness
           material, don't add them again. Has to be done in a separate pass
           to avoid resetting too early. */
        for(const MaterialAttributeData& attribute: attributes) {
            if(attribute.name() == "DiffuseColor")
                diffuseColor = Containers::NullOpt;
            else if(attribute.name() == "DiffuseTexture")
                diffuseTexture = Containers::NullOpt;
            else if(attribute.name() == "DiffuseTextureMatrix")
                diffuseTextureMatrix = Containers::NullOpt;
            else if(attribute.name() == "DiffuseTextureCoordinates")
                diffuseTextureCoordinates = Containers::NullOpt;
        }

        if(diffuseColor)
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::DiffuseColor, *diffuseColor);
        if(diffuseTexture)
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::DiffuseTexture, *diffuseTexture);
        if(diffuseTextureMatrix)
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::DiffuseTextureMatrix, *diffuseTextureMatrix);
        if(diffuseTextureCoordinates)
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::DiffuseTextureCoordinates, *diffuseTextureCoordinates);
    }

    /* Clear coat layer -- needs to be after all base material attributes */
    auto khrMaterialsClearCoat = material.extensions.find("KHR_materials_clearcoat");
    if(khrMaterialsClearCoat != material.extensions.end()) {
        types |= MaterialType::PbrClearCoat;

        /* Add a new layer -- this works both if layers are empty and if
           there's something already */
        arrayAppend(layers, UnsignedInt(attributes.size()));
        arrayAppend(attributes, InPlaceInit, MaterialLayer::ClearCoat);

        auto clearcoatFactor = khrMaterialsClearCoat->second.Get("clearcoatFactor");
        if(clearcoatFactor.Type() != tinygltf::NULL_TYPE) {
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::LayerFactor,
                Float(clearcoatFactor.Get<double>()));
        } else {
            /* Default factor is 0, i.e. the layer disabled. I assume this is
               in order to be ready for when clearcoat is part of the material
               object and not an extension (in which case the values would be
               always present and thus it make sense to have clearcoat layers
               disabled by default). Original reasoning here:
               https://github.com/KhronosGroup/glTF/pull/1677#issuecomment-543268157

               In our MaterialData API the presence of the layer alone enables
               it and thus a default of 1 makes sense -- so one can specify
               just the texture, without the factor as well. */
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::LayerFactor, 0.0f);
        }

        auto clearcoatTexture = khrMaterialsClearCoat->second.Get("clearcoatTexture");
        if(clearcoatTexture.Type() != tinygltf::NULL_TYPE) {
            if(!materialTexture("clearcoatTexture",
                clearcoatTexture.Get("index").Get<int>(),
                clearcoatTexture.Get("texCoord").Get<int>(),
                clearcoatTexture.Get("extensions"),
                attributes,
                MaterialAttribute::LayerFactorTexture,
                MaterialAttribute::LayerFactorTextureMatrix,
                MaterialAttribute::LayerFactorTextureCoordinates)
            )
                return Containers::NullOpt;
        }

        auto clearcoatRoughnessFactor = khrMaterialsClearCoat->second.Get("clearcoatRoughnessFactor");
        if(clearcoatRoughnessFactor.Type() != tinygltf::NULL_TYPE) {
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::Roughness,
                Float(clearcoatRoughnessFactor.Get<double>()));
        } else {
            /* Default factor in glTF is 0, not 1. I assume there's a similar
               reasoning as with the clearcoatFactor above, but it makes less
               sense. */
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::Roughness, 0.0f);
        }

        auto clearcoatRoughnessTexture = khrMaterialsClearCoat->second.Get("clearcoatRoughnessTexture");
        if(clearcoatRoughnessTexture.Type() != tinygltf::NULL_TYPE) {
            if(!materialTexture("clearcoatRoughnessTexture",
                clearcoatRoughnessTexture.Get("index").Get<int>(),
                clearcoatRoughnessTexture.Get("texCoord").Get<int>(),
                clearcoatRoughnessTexture.Get("extensions"),
                attributes,
                MaterialAttribute::RoughnessTexture,
                MaterialAttribute::RoughnessTextureMatrix,
                MaterialAttribute::RoughnessTextureCoordinates)
            )
                return Containers::NullOpt;

            /* The extension description doesn't mention it, but the schema
               says the clearcoat roughness is actually in the G channel:
               https://github.com/KhronosGroup/glTF/blob/dc5519b9ce9834f07c30ec4c957234a0cd6280a2/extensions/2.0/Khronos/KHR_materials_clearcoat/schema/glTF.KHR_materials_clearcoat.schema.json#L32 */
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::RoughnessTextureSwizzle,
                MaterialTextureSwizzle::G);
        }

        auto clearcoatNormalTexture = khrMaterialsClearCoat->second.Get("clearcoatNormalTexture");
        if(clearcoatNormalTexture.Type() != tinygltf::NULL_TYPE) {
            if(!materialTexture("clearcoatNormalTexture",
                clearcoatNormalTexture.Get("index").Get<int>(),
                clearcoatNormalTexture.Get("texCoord").Get<int>(),
                clearcoatNormalTexture.Get("extensions"),
                attributes,
                MaterialAttribute::NormalTexture,
                MaterialAttribute::NormalTextureMatrix,
                MaterialAttribute::NormalTextureCoordinates)
            )
                return Containers::NullOpt;

            auto scale = clearcoatNormalTexture.Get("scale");
            if(scale.Type() != tinygltf::NULL_TYPE) {
                arrayAppend(attributes, InPlaceInit,
                    MaterialAttribute::NormalTextureScale,
                    Float(scale.Get<double>()));
            }
        }
    }

    /* If there's any layer, add the final attribute count */
    arrayAppend(layers, UnsignedInt(attributes.size()));

    /* Can't use growable deleters in a plugin, convert back to the default
       deleter */
    arrayShrink(layers);
    arrayShrink(attributes, ValueInit);
    return MaterialData{types, std::move(attributes), std::move(layers), &material};
}

UnsignedInt TinyGltfImporter::doTextureCount() const {
    return _d->model.textures.size();
}

Int TinyGltfImporter::doTextureForName(const Containers::StringView name) {
    if(!_d->texturesForName) {
        _d->texturesForName.emplace();
        _d->texturesForName->reserve(_d->model.textures.size());
        for(std::size_t i = 0; i != _d->model.textures.size(); ++i)
            _d->texturesForName->emplace(_d->model.textures[i].name, i);
    }

    const auto found = _d->texturesForName->find(name);
    return found == _d->texturesForName->end() ? -1 : found->second;
}

Containers::String TinyGltfImporter::doTextureName(const UnsignedInt id) {
    return _d->model.textures[id].name;
}

Containers::Optional<TextureData> TinyGltfImporter::doTexture(const UnsignedInt id) {
    const tinygltf::Texture& tex = _d->model.textures[id];

    UnsignedInt imageId = ~0u;

    using namespace Containers::Literals;

    /* Various extensions, they override the standard image */
    constexpr Containers::StringView extensions[]{
        /* Allows the usage of mimeType image/ktx2 but only explicitly talks
           about KTX2 with Basis compression. We don't care since we delegate
           to AnyImageImporter and let it figure out the file type based on
           magic. However, in case of embedded images we *have to* use
           data:application/octet-stream there because TinyGLTF has a whitelist
           for MIME types and the bundled version doesn't know image/ktx2:
           https://github.com/syoyo/tinygltf/blob/7e009041e35b999fd1e47c0f0e42cadcf8f5c31c/tiny_gltf.h#L2706 */
        "KHR_texture_basisu"_s,
        /* This is not a registered extension but can be found in some of the
           early Basis Universal examples. Basis files don't have a registered
           mimetype either, but as explained above we don't care about mimetype
           at all. Same issue with embedded images applies because even if
           there was a MIME type, TinyGLTF wouldn't know it. */
        "GOOGLE_texture_basis"_s
    };

    for(const auto& ext: extensions) {
        const auto found = tex.extensions.find(ext);
        if(found != tex.extensions.end()) {
            const int source = found->second.Get("source").Get<int>();
            if(UnsignedInt(source) >= _d->model.images.size()) {
                Error{} << "Trade::TinyGltfImporter::texture():" << ext << "image" << source << "out of range for" << _d->model.images.size() << "images";
                return Containers::NullOpt;
            }
            imageId = source;
            break;
        }
    }

    if(imageId == ~0u) {
        /* If not overwritten by an extension, use the standard 'source'
           attribute. It's not mandatory, so this can still fail. */
        if(tex.source != -1) {
            if(UnsignedInt(tex.source) >= _d->model.images.size()) {
                Error{} << "Trade::TinyGltfImporter::texture(): image" << tex.source << "out of range for" << _d->model.images.size() << "images";
                return Containers::NullOpt;
            }

            imageId = UnsignedInt(tex.source);
        } else {
            Error{} << "Trade::TinyGltfImporter::texture(): no image source found";
            return Containers::NullOpt;
        }
    }

    CORRADE_INTERNAL_ASSERT(imageId < _d->model.images.size());

    /* Sampler */
    if(tex.sampler == -1) {
        /* The specification instructs to use "auto sampling", i.e. it is left
           to the implementor to decide on the default values... */
        return TextureData{TextureType::Texture2D, SamplerFilter::Linear, SamplerFilter::Linear,
            SamplerMipmap::Linear, {SamplerWrapping::Repeat, SamplerWrapping::Repeat, SamplerWrapping::Repeat}, imageId, &tex};
    }

    if(UnsignedInt(tex.sampler) >= _d->model.samplers.size()) {
        Error{} << "Trade::TinyGltfImporter::texture(): sampler" << tex.sampler << "out of range for" << _d->model.samplers.size() << "samplers";
        return Containers::NullOpt;
    }

    const tinygltf::Sampler& s = _d->model.samplers[tex.sampler];

    SamplerFilter minFilter;
    SamplerMipmap mipmap;
    switch(s.minFilter) {
        case TINYGLTF_TEXTURE_FILTER_NEAREST:
            minFilter = SamplerFilter::Nearest;
            mipmap = SamplerMipmap::Base;
            break;
        case TINYGLTF_TEXTURE_FILTER_LINEAR:
            minFilter = SamplerFilter::Linear;
            mipmap = SamplerMipmap::Base;
            break;
        case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST:
            minFilter = SamplerFilter::Nearest;
            mipmap = SamplerMipmap::Nearest;
            break;
        case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR:
            minFilter = SamplerFilter::Nearest;
            mipmap = SamplerMipmap::Linear;
            break;
        case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST:
            minFilter = SamplerFilter::Linear;
            mipmap = SamplerMipmap::Nearest;
            break;
        case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR:
        /* glTF 2.0 spec does not define a default value for 'minFilter' and
           `magFilter`. In this case tinygltf sets it to -1
           (see https://github.com/syoyo/tinygltf/issues/186) */
        case -1:
            minFilter = SamplerFilter::Linear;
            mipmap = SamplerMipmap::Linear;
            break;
        default:
            Error{} << "Trade::TinyGltfImporter::texture(): invalid minFilter" << s.minFilter;
            return Containers::NullOpt;
    }

    SamplerFilter magFilter;
    switch(s.magFilter) {
        case TINYGLTF_TEXTURE_FILTER_NEAREST:
            magFilter = SamplerFilter::Nearest;
            break;
        case TINYGLTF_TEXTURE_FILTER_LINEAR:
        /* glTF 2.0 spec does not define a default value for 'minFilter' and
           `magFilter`. In this case tinygltf sets it to -1
           (see https://github.com/syoyo/tinygltf/issues/186) */
        case -1:
            magFilter = SamplerFilter::Linear;
            break;
        default:
            Error{} << "Trade::TinyGltfImporter::texture(): invalid magFilter" << s.magFilter;
            return Containers::NullOpt;
    }

    /* There's wrapR that is a tiny_gltf extension and is set to zero. Ignoring
       that one and hardcoding it to Repeat. */
    Math::Vector3<SamplerWrapping> wrapping;
    wrapping.z() = SamplerWrapping::Repeat;
    for(auto&& wrap: std::initializer_list<std::pair<int, int>>{
        {s.wrapS, 0}, {s.wrapT, 1}})
    {
        switch(wrap.first) {
            case TINYGLTF_TEXTURE_WRAP_REPEAT:
                wrapping[wrap.second] = SamplerWrapping::Repeat;
                break;
            case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE:
                wrapping[wrap.second] = SamplerWrapping::ClampToEdge;
                break;
            case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT:
                wrapping[wrap.second] = SamplerWrapping::MirroredRepeat;
                break;
            default:
                Error{} << "Trade::TinyGltfImporter::texture(): invalid wrap mode" << wrap.first;
                return Containers::NullOpt;
        }
    }

    /* glTF supports only 2D textures */
    return TextureData{TextureType::Texture2D, minFilter, magFilter,
        mipmap, wrapping, imageId, &tex};
}

UnsignedInt TinyGltfImporter::doImage2DCount() const {
    return _d->model.images.size();
}

Int TinyGltfImporter::doImage2DForName(const Containers::StringView name) {
    if(!_d->imagesForName) {
        _d->imagesForName.emplace();
        _d->imagesForName->reserve(_d->model.images.size());
        for(std::size_t i = 0; i != _d->model.images.size(); ++i)
            _d->imagesForName->emplace(_d->model.images[i].name, i);
    }

    const auto found = _d->imagesForName->find(name);
    return found == _d->imagesForName->end() ? -1 : found->second;
}

Containers::String TinyGltfImporter::doImage2DName(const UnsignedInt id) {
    return _d->model.images[id].name;
}

AbstractImporter* TinyGltfImporter::setupOrReuseImporterForImage(const UnsignedInt id, const char* const errorPrefix) {
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

    /* Because we specified an empty callback for loading image data,
       Image.image, Image.width, Image.height and Image.component will not be
       valid and should not be accessed. */

    const tinygltf::Image& image = _d->model.images[id];

    AnyImageImporter importer{*manager()};
    if(fileCallback()) importer.setFileCallback(fileCallback(), fileCallbackUserData());

    /* Load embedded image */
    if(image.uri.empty()) {
        Containers::ArrayView<const char> data;

        /* The image data are stored in a buffer */
        if(image.bufferView != -1) {
            const tinygltf::BufferView& bufferView = _d->model.bufferViews[image.bufferView];
            const tinygltf::Buffer& buffer = _d->model.buffers[bufferView.buffer];

            data = Containers::arrayCast<const char>(Containers::arrayView(&buffer.data[bufferView.byteOffset], bufferView.byteLength));

        /* Image data were a data URI, the loadImageData() callback copied them
           without decoding to the internal data vector */
        } else {
            data = Containers::arrayCast<const char>(Containers::arrayView(image.image.data(), image.image.size()));
        }

        Containers::Optional<ImageData2D> imageData;
        if(!importer.openData(data))
            return nullptr;
        return &_d->imageImporter.emplace(std::move(importer));
    }

    /* Load external image */
    if(!_d->filePath && !fileCallback()) {
        Error{} << errorPrefix << "external images can be imported only when opening files from the filesystem or if a file callback is present";
        return nullptr;
    }

    Containers::Optional<ImageData2D> imageData;
    if(!importer.openFile(Utility::Path::join(_d->filePath ? *_d->filePath : "", image.uri)))
        return nullptr;
    return &_d->imageImporter.emplace(std::move(importer));
}

UnsignedInt TinyGltfImporter::doImage2DLevelCount(const UnsignedInt id) {
    CORRADE_ASSERT(manager(), "Trade::TinyGltfImporter::image2DLevelCount(): the plugin must be instantiated with access to plugin manager in order to open image files", {});

    AbstractImporter* importer = setupOrReuseImporterForImage(id, "Trade::TinyGltfImporter::image2DLevelCount():");
    /* image2DLevelCount() isn't supposed to fail (image2D() is, instead), so
       report 1 on failure and expect image2D() to fail later */
    if(!importer) return 1;

    return importer->image2DLevelCount(0);
}

Containers::Optional<ImageData2D> TinyGltfImporter::doImage2D(const UnsignedInt id, const UnsignedInt level) {
    CORRADE_ASSERT(manager(), "Trade::TinyGltfImporter::image2D(): the plugin must be instantiated with access to plugin manager in order to load images", {});

    AbstractImporter* importer = setupOrReuseImporterForImage(id, "Trade::TinyGltfImporter::image2D():");
    if(!importer) return Containers::NullOpt;

    /* Include a pointer to the tinygltf state in the result */
    Containers::Optional<ImageData2D> imageData = importer->image2D(0, level);
    if(!imageData) return Containers::NullOpt;
    return ImageData2D{std::move(*imageData), &_d->model.images[id]};
}

const void* TinyGltfImporter::doImporterState() const {
    return &_d->model;
}

}}

CORRADE_IGNORE_DEPRECATED_PUSH
CORRADE_PLUGIN_REGISTER(TinyGltfImporter, Magnum::Trade::TinyGltfImporter,
    MAGNUM_TRADE_ABSTRACTIMPORTER_PLUGIN_INTERFACE)
CORRADE_IGNORE_DEPRECATED_POP
