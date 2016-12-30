#pragma once

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "glm/glm.hpp"
#include "glm/gtx/hash.hpp"

#include "gli/gli.hpp"
#include "gli/convert.hpp"
#include "gli/generate_mipmaps.hpp"

#include "assimp/Importer.hpp"
#include "assimp/scene.h"    
#include "assimp/postprocess.h"
#include "assimp/cimport.h"
#include "VManager.h"


#define DIFF_IRRADIANCE_MAP_SIZE 32
#define SPEC_IRRADIANCE_MAP_SIZE 512


struct Vertex
{
	glm::vec3 pos;
	glm::vec3 normal;
	glm::vec2 texCoord;

	bool operator==(const Vertex& other) const
	{
		return pos == other.pos &&
			normal == other.normal &&
			texCoord == other.texCoord;
	}

	static VkVertexInputBindingDescription getBindingDescription()
	{
		VkVertexInputBindingDescription bindingDescription = {};
		bindingDescription.binding = 0;
		bindingDescription.stride = sizeof(Vertex);
		bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		return bindingDescription;
	}

	static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions()
	{
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions(3, {});

		attributeDescriptions[0].binding = 0;
		attributeDescriptions[0].location = 0;
		attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
		attributeDescriptions[0].offset = offsetof(Vertex, pos);

		attributeDescriptions[1].binding = 0;
		attributeDescriptions[1].location = 1;
		attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
		attributeDescriptions[1].offset = offsetof(Vertex, normal);

		attributeDescriptions[2].binding = 0;
		attributeDescriptions[2].location = 2;
		attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
		attributeDescriptions[2].offset = offsetof(Vertex, texCoord);

		return attributeDescriptions;
	}
};

struct ImageWrapper
{
	uint32_t image;
	std::vector<uint32_t> imageViews;

	VkFormat format;
	uint32_t width, height, depth = 1;
	uint32_t mipLevelCount;
};

namespace std
{
	template<> struct hash<Vertex>
	{
		size_t operator()(Vertex const& vertex) const
		{
			using namespace rj::helper_functions;
			size_t seed = 0;
			hash_combine(seed, vertex.pos);
			hash_combine(seed, vertex.normal);
			hash_combine(seed, vertex.texCoord);
			return seed;
		}
	};
}

namespace rj
{
	namespace helper_functions
	{
		void loadMeshIntoHostBuffers(const std::string &modelFileName,
			std::vector<Vertex> &hostVerts, std::vector<uint32_t> &hostIndices)
		{
			Assimp::Importer meshImporter;
			const aiScene *scene = nullptr;

			const uint32_t defaultFlags =
				aiProcess_FlipWindingOrder |
				aiProcess_Triangulate |
				aiProcess_PreTransformVertices |
				aiProcess_GenSmoothNormals;

			scene = meshImporter.ReadFile(modelFileName, defaultFlags);

			std::unordered_map<Vertex, uint32_t> vert2IdxLut;

			for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
			{
				const aiMesh *mesh = scene->mMeshes[i];
				const aiVector3D *vertices = mesh->mVertices;
				const aiVector3D *normals = mesh->mNormals;
				const aiVector3D *texCoords = mesh->mTextureCoords[0];
				const auto *faces = mesh->mFaces;

				if (!normals || !texCoords)
				{
					throw std::runtime_error("model must have normals and uvs.");
				}

				for (uint32_t j = 0; j < mesh->mNumFaces; ++j)
				{
					const aiFace &face = faces[j];

					if (face.mNumIndices != 3) continue;

					for (uint32_t k = 0; k < face.mNumIndices; ++k)
					{
						uint32_t idx = face.mIndices[k];
						const aiVector3D &pos = vertices[idx];
						const aiVector3D &nrm = normals[idx];
						const aiVector3D &texCoord = texCoords[idx];

						Vertex vert =
						{
							glm::vec3(pos.x, pos.y, pos.z),
							glm::vec3(nrm.x, nrm.y, nrm.z),
							glm::vec2(texCoord.x, 1.f - texCoord.y)
						};

						const auto searchResult = vert2IdxLut.find(vert);
						if (searchResult == vert2IdxLut.end())
						{
							uint32_t newIdx = static_cast<uint32_t>(hostVerts.size());
							vert2IdxLut[vert] = newIdx;
							hostIndices.emplace_back(newIdx);
							hostVerts.emplace_back(vert);
						}
						else
						{
							hostIndices.emplace_back(searchResult->second);
						}
					}
				}
			}
		}

		void loadTexture2D(ImageWrapper *pTexRet, VManager *pManager, const std::string &fn, bool generateMipLevels = true)
		{
			std::string ext = getFileExtension(fn);
			if (ext != "ktx" && ext != "dds")
			{
				throw std::runtime_error("texture type ." + ext + " is not supported.");
			}

			gli::texture2d textureSrc(gli::load(fn.c_str()));

			if (textureSrc.empty())
			{
				throw std::runtime_error("cannot load texture.");
			}

			gli::texture2d textureMipmapped;
			if (textureSrc.levels() == 1 && generateMipLevels)
			{
				textureMipmapped = gli::generate_mipmaps(textureSrc, gli::FILTER_LINEAR);
			}
			else
			{
				textureMipmapped = textureSrc;
			}

			VkFormat format;
			switch (textureMipmapped.format())
			{
			case gli::FORMAT_RGBA8_UNORM_PACK8:
				format = VK_FORMAT_R8G8B8A8_UNORM;
				break;
			case gli::FORMAT_RGBA32_SFLOAT_PACK32:
				format = VK_FORMAT_R32G32B32A32_SFLOAT;
				break;
			case gli::FORMAT_RGBA_DXT5_UNORM_BLOCK16:
				format = VK_FORMAT_BC3_UNORM_BLOCK;
				break;
			case gli::FORMAT_RG32_SFLOAT_PACK32:
				format = VK_FORMAT_R32G32_SFLOAT;
				break;
			default:
				throw std::runtime_error("texture format is not supported.");
			}

			uint32_t width = static_cast<uint32_t>(textureMipmapped.extent().x);
			uint32_t height = static_cast<uint32_t>(textureMipmapped.extent().y);
			uint32_t mipLevels = static_cast<uint32_t>(textureMipmapped.levels());
			size_t sizeInBytes = textureMipmapped.size();

			pTexRet->width = width;
			pTexRet->height = height;
			pTexRet->format = format;
			pTexRet->mipLevelCount = mipLevels;

			pTexRet->image = pManager->createImage2D(width, height, format,
				VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				mipLevels);

			pManager->transferHostDataToImage(pTexRet->image, sizeInBytes, textureMipmapped.data(),
				VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

			pTexRet->imageViews.push_back(pManager->createImageView2D(pTexRet->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels));
		}
	}
}

enum MaterialType
{
	MATERIAL_TYPE_HDR_PROBE = 0,
	MATERIAL_TYPE_FSCHLICK_DGGX_GSMITH,
	MATERIAL_TYPE_COUNT
};

typedef uint32_t MaterialType_t;

struct PerModelUniformBuffer
{
	glm::mat4 M;
	glm::mat4 M_invTrans;
};

class VMesh
{
public:
	const static uint32_t numMapsPerMesh = 5;

	rj::VManager *pVulkanManager;

	PerModelUniformBuffer *uPerModelInfo = nullptr;

	bool uniformDataChanged = true;
	glm::vec3 worldPosition{ 0.f, 0.f, 0.f };
	glm::quat worldRotation{ glm::vec3(0.f, 0.f, 0.f) }; // Euler angles to quaternion
	float scale = 1.f;

	uint32_t vertexBuffer;
	uint32_t indexBuffer;

	ImageWrapper albedoMap;
	ImageWrapper normalMap;
	ImageWrapper roughnessMap;
	ImageWrapper metalnessMap;
	ImageWrapper aoMap;

	MaterialType_t materialType = MATERIAL_TYPE_FSCHLICK_DGGX_GSMITH;


	VMesh(rj::VManager *pManager) : pVulkanManager(pManager)
	{}

	void load(
		const std::string &modelFileName,
		const std::string &albedoMapName,
		const std::string &normalMapName,
		const std::string &roughnessMapName,
		const std::string &metalnessMapName,
		const std::string &aoMapName = "")
	{
		using namespace rj::helper_functions;

		// load textures
		if (albedoMapName != "")
		{
			loadTexture2D(&albedoMap, pVulkanManager, albedoMapName);
		}
		if (normalMapName != "")
		{
			loadTexture2D(&normalMap, pVulkanManager, normalMapName);
		}
		if (roughnessMapName != "")
		{
			loadTexture2D(&roughnessMap, pVulkanManager, roughnessMapName);
		}
		if (metalnessMapName != "")
		{
			loadTexture2D(&metalnessMap, pVulkanManager, metalnessMapName);
		}
		if (aoMapName != "")
		{
			loadTexture2D(&aoMap, pVulkanManager, aoMapName);
		}

		// load mesh
		std::vector<Vertex> hostVerts;
		std::vector<uint32_t> hostIndices;
		loadMeshIntoHostBuffers(modelFileName, hostVerts, hostIndices);

		// create vertex buffer
		createBufferFromHostData(
			physicalDevice, device, commandPool, submitQueue,
			hostVerts.data(), hostVerts.size(), sizeof(hostVerts[0]) * hostVerts.size(),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			vertexBuffer);

		// create index buffer
		createBufferFromHostData(
			physicalDevice, device, commandPool, submitQueue,
			hostIndices.data(), hostIndices.size(), sizeof(hostIndices[0]) * hostIndices.size(),
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			indexBuffer);
	}

	virtual void updateHostUniformBuffer()
	{
		assert(uPerModelInfo);
		if (!uniformDataChanged) return;
		uPerModelInfo->M = glm::translate(glm::mat4_cast(worldRotation) * glm::scale(glm::mat4(), glm::vec3(scale)), worldPosition);
		uPerModelInfo->M_invTrans = glm::transpose(glm::inverse(uPerModelInfo->M));
		uniformDataChanged = false;
	}
};

class Skybox : public VMesh
{
public:
	ImageWrapper radianceMap; // unfiltered map
	ImageWrapper specularIrradianceMap;
	ImageWrapper diffuseIrradianceMap; // TODO: use spherical harmonics instead

	bool specMapReady = false;
	bool diffMapReady = false;
	bool shouldSaveSpecMap = false;
	bool shouldSaveDiffMap = false;

	Skybox(const VDeleter<VkDevice> &device) :
		VMesh{ device },
		radianceMap{ device, VK_FORMAT_R32G32_SFLOAT },
		specularIrradianceMap{ device, VK_FORMAT_R32G32B32A32_SFLOAT },
		diffuseIrradianceMap{ device, VK_FORMAT_R32G32B32A32_SFLOAT }
	{
		materialType = MATERIAL_TYPE_HDR_PROBE;
	}

	void load(
		VkPhysicalDevice physicalDevice,
		const VDeleter<VkDevice> &device,
		VkCommandPool commandPool,
		VkQueue submitQueue,
		const std::string &modelFileName,
		const std::string &radianceMapName,
		const std::string &specMapName,
		const std::string &diffuseMapName)
	{
		if (radianceMapName != "")
		{
			loadCubemap(physicalDevice, device, commandPool, submitQueue, radianceMapName, radianceMap);
		}
		else
		{
			throw std::invalid_argument("radiance map required but not provided.");
		}

		if (specMapName != "")
		{
			loadCubemap(physicalDevice, device, commandPool, submitQueue, specMapName, specularIrradianceMap);
			specMapReady = true;
		}
		else
		{
			uint32_t mipLevels = static_cast<uint32_t>(floor(log2f(SPEC_IRRADIANCE_MAP_SIZE) + 0.5f)) + 1;
			specularIrradianceMap.mipLevels = mipLevels;
			specularIrradianceMap.format = VK_FORMAT_R32G32B32A32_SFLOAT;

			createCubemapImage(physicalDevice, device,
				SPEC_IRRADIANCE_MAP_SIZE, SPEC_IRRADIANCE_MAP_SIZE,
				mipLevels,
				specularIrradianceMap.format, VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				specularIrradianceMap.image, specularIrradianceMap.imageMemory);

			createImageViewCube(device, specularIrradianceMap.image, specularIrradianceMap.format,
				VK_IMAGE_ASPECT_COLOR_BIT, specularIrradianceMap.mipLevels, specularIrradianceMap.imageView);

			specularIrradianceMap.imageViews.resize(mipLevels, { device, vkDestroyImageView });
			for (uint32_t level = 0; level < mipLevels; ++level)
			{
				createImageViewCube(device, specularIrradianceMap.image, specularIrradianceMap.format,
					VK_IMAGE_ASPECT_COLOR_BIT, level, 1, specularIrradianceMap.imageViews[level]);
			}

			VkSamplerCreateInfo samplerInfo = {};
			getDefaultSamplerCreateInfo(samplerInfo);
			samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.maxLod = static_cast<float>(specularIrradianceMap.mipLevels - 1);

			if (vkCreateSampler(device, &samplerInfo, nullptr, specularIrradianceMap.sampler.replace()) != VK_SUCCESS)
			{
				throw std::runtime_error("failed to create specular irradiance map sampler!");
			}

			shouldSaveSpecMap = true;
		}

		if (diffuseMapName != "")
		{
			loadCubemap(physicalDevice, device, commandPool, submitQueue, diffuseMapName, diffuseIrradianceMap);
			diffMapReady = true;
		}
		else
		{
			diffuseIrradianceMap.mipLevels = 1;
			diffuseIrradianceMap.format = VK_FORMAT_R32G32B32A32_SFLOAT;

			createCubemapImage(physicalDevice, device,
				DIFF_IRRADIANCE_MAP_SIZE, DIFF_IRRADIANCE_MAP_SIZE,
				diffuseIrradianceMap.mipLevels,
				diffuseIrradianceMap.format, VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				diffuseIrradianceMap.image, diffuseIrradianceMap.imageMemory);

			createImageViewCube(device, diffuseIrradianceMap.image, diffuseIrradianceMap.format,
				VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, diffuseIrradianceMap.imageView);

			VkSamplerCreateInfo samplerInfo = {};
			getDefaultSamplerCreateInfo(samplerInfo);
			samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.maxLod = static_cast<float>(diffuseIrradianceMap.mipLevels - 1);

			if (vkCreateSampler(device, &samplerInfo, nullptr, diffuseIrradianceMap.sampler.replace()) != VK_SUCCESS)
			{
				throw std::runtime_error("failed to create diffuse irradiance map sampler!");
			}

			shouldSaveDiffMap = true;
		}

		VMesh::load(physicalDevice, device, commandPool, submitQueue, modelFileName, "", "", "", "");
	}
};