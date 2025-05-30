﻿#include <filesystem>
#include <iostream>
#include <fbxsdk.h>

#include "Core.h"
#include "UnCore.h"
#include "ExportFBX.h"

#include "UnObject.h"
#include "Unreal/UnrealMaterial/UnMaterial.h"
#include "Unreal/Mesh/StaticMesh.h"
#include "Exporters.h"

#ifdef IOS_REF
#undef  IOS_REF
#define IOS_REF (*(SdkManager->GetIOSettings()))
#endif

const char* FBXVersions[] =
{
	FBX_2010_00_COMPATIBLE,
	FBX_2011_00_COMPATIBLE,
	FBX_2012_00_COMPATIBLE,
	FBX_2013_00_COMPATIBLE,
	FBX_2014_00_COMPATIBLE,
	FBX_2016_00_COMPATIBLE,
	FBX_2018_00_COMPATIBLE,
	FBX_2019_00_COMPATIBLE,
	FBX_2020_00_COMPATIBLE
};

const char* GetExportTextureName(const UUnrealMaterial *Tex)
{
	CTextureData tex_data;
	// For HDR textures use Radiance format
	if (PixelFormatInfo[tex_data.Format].Float)
	{
		return GetExportFileName(Tex, "%s.hdr", Tex->Name);
	}
	return GetExportFileName(Tex, "%s.tga", Tex->Name);
}

FFbxExportOption::FFbxExportOption()
{
	FbxExportCompatibility = EFbxExportCompatibility::FBX_2018;
	bForceFrontXAxis = false;
	LevelOfDetail = true;
	Collision = true;
	VertexColor = true;
	WeldedVertices = true;
	MapSkeletalMotionToRoot = false;
}

void FFbxExporter::CreateDocument()
{
	Scene = FbxScene::Create(SdkManager, "");

	// create scene info
	FbxDocumentInfo* SceneInfo = FbxDocumentInfo::Create(SdkManager, "SceneInfo");
	SceneInfo->mTitle = "Unreal FBX Exporter";
	SceneInfo->mSubject = "Export FBX meshes from Unreal";
	SceneInfo->mAuthor = "UModel";
	Scene->SetSceneInfo(SceneInfo);

	//FbxScene->GetGlobalSettings().SetOriginalUpAxis(KFbxAxisSystem::Max);
	FbxAxisSystem::EFrontVector FrontVector = static_cast<FbxAxisSystem::EFrontVector>(-FbxAxisSystem::eParityOdd);
	if (ExportOptions->bForceFrontXAxis)
		FrontVector = FbxAxisSystem::eParityEven;

	const FbxAxisSystem UnrealZUp(FbxAxisSystem::eZAxis, FrontVector, FbxAxisSystem::eRightHanded);
	Scene->GetGlobalSettings().SetAxisSystem(UnrealZUp);
	Scene->GetGlobalSettings().SetOriginalUpAxis(UnrealZUp);
	// Maya use cm by default
	Scene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::cm);
	//FbxScene->GetGlobalSettings().SetOriginalSystemUnit( KFbxSystemUnit::m );

	// setup anim stack
	AnimStack = FbxAnimStack::Create(Scene, "Unreal Take");
	//KFbxSet<KTime>(AnimStack->LocalStart, KTIME_ONE_SECOND);
	AnimStack->Description.Set("Animation Take for Unreal.");

	// this take contains one base layer. In fact having at least one layer is mandatory.
	AnimLayer = FbxAnimLayer::Create(Scene, "Base Layer");
	AnimStack->AddMember(AnimLayer);
}

void FFbxExporter::CloseDocument()
{
	//FbxSkeletonRoots.Reset();
	FbxMaterials.Reset();
	FbxMeshes.Reset();
	//FbxNodeNameToIndexMap.Reset();
	if (Scene)
	{
		Scene->Destroy();
		Scene = NULL;
	}
}

namespace fs = std::filesystem;
void FFbxExporter::WriteToFile(const char* Filename)
{
	FbxExporter* exporter = FbxExporter::Create(SdkManager, "");
	
	IOS_REF.SetBoolProp(EXP_FBX_MATERIAL, true);
	IOS_REF.SetBoolProp(EXP_FBX_TEXTURE, true);
	IOS_REF.SetBoolProp(EXP_FBX_EMBEDDED, false);
	IOS_REF.SetBoolProp(EXP_FBX_SHAPE, true);
	IOS_REF.SetBoolProp(EXP_FBX_GOBO, true);
	IOS_REF.SetBoolProp(EXP_FBX_ANIMATION, true);
	IOS_REF.SetBoolProp(EXP_FBX_GLOBAL_SETTINGS, true);
	
	// Initialize the exporter by providing a filename.
	if (!exporter->Initialize(Filename, -1, SdkManager->GetIOSettings()))
	{
		appPrintf("Call to KFbxExporter::Initialize() failed.\n");
		appPrintf("Error returned: %s\n\n", exporter->GetStatus().GetErrorString());
		return;
	}

	// Export the scene.
	const fs::path file_path = Filename;
	if (const auto dir_path = file_path.parent_path(); fs::exists(dir_path) || fs::create_directories(dir_path))
	{
		if (const bool is_success = exporter->Export(Scene); !is_success)
		{
			appPrintf("FBX Export error: %s\n", exporter->GetStatus().GetErrorString());
		}
	}

	// Destroy the exporter.
	exporter->Destroy();

	CloseDocument();
}

void FFbxExporter::ExportStaticMesh(const CStaticMesh* StaticMesh)
{
	FString MeshName = StaticMesh->OriginalMesh->Name;
	FbxNode* MeshNode = FbxNode::Create(Scene, *MeshName);
	Scene->GetRootNode()->AddChild(MeshNode);

	ExportOptions->LevelOfDetail = GExportLods;
	if (ExportOptions->LevelOfDetail && StaticMesh->Lods.Num() > 1)
	{
		char LodGroup_MeshName[128];
		appSprintf(LodGroup_MeshName, 128, "%s_LodGroup", *MeshName);
		FbxLODGroup* FbxLodGroupAttribute = FbxLODGroup::Create(Scene, LodGroup_MeshName);
		MeshNode->AddNodeAttribute(FbxLodGroupAttribute);
		FbxLodGroupAttribute->ThresholdsUsedAsPercentage = true;
		//Export an Fbx Mesh Node for every LOD and child them to the fbx node (LOD Group)
		for (int CurrentLodIndex = 0; CurrentLodIndex < StaticMesh->Lods.Num(); ++CurrentLodIndex)
		{
			char NodeName[512];
			appSprintf(ARRAY_ARG(NodeName), "%s_LOD%d", *MeshName, CurrentLodIndex);
			FString FbxLODNodeName = NodeName;
			FbxNode* FbxActorLOD = FbxNode::Create(Scene, *FbxLODNodeName);
			MeshNode->AddChild(FbxActorLOD);
			if (CurrentLodIndex + 1 < StaticMesh->Lods.Num())
			{
				//Convert the screen size to a threshold, it is just to be sure that we set some threshold, there is no way to convert this precisely
				double LodScreenSize = 10.0f * (CurrentLodIndex + 1);
				FbxLodGroupAttribute->AddThreshold(LodScreenSize);
			}
			ExportStaticMeshToFbx(StaticMesh, CurrentLodIndex, *MeshName, FbxActorLOD, -1);
		}
	}
	else
	{
		ExportStaticMeshToFbx(StaticMesh, 0, *MeshName, MeshNode, -1);
	}
}

FFbxExporter::FFbxExporter()
{
	ExportOptions = new FFbxExportOption();

	// Create the SdkManager
	SdkManager = FbxManager::Create();

	// create an IOSettings object
	FbxIOSettings * ios = FbxIOSettings::Create(SdkManager, IOSROOT);
	SdkManager->SetIOSettings(ios);

	Scene = nullptr;
	AnimStack = nullptr;
	AnimLayer = nullptr;
	DefaultCamera = nullptr;
}

FFbxExporter::~FFbxExporter()
{
	if (SdkManager)
	{
		SdkManager->Destroy();
		SdkManager = nullptr;
	}
	if (ExportOptions)
	{
		delete ExportOptions;
		ExportOptions = nullptr;
	}
}

void DetermineVertsToWeld(TArray<int32>& VertRemap, TArray<int32>& UniqueVerts, const CStaticMeshLod& RenderMesh)
{
	const int32 VertexCount = RenderMesh.NumVerts;

	// Maps unreal verts to reduced list of verts
	VertRemap.Empty(VertexCount);
	VertRemap.AddUninitialized(VertexCount);

	// List of Unreal Verts to keep
	UniqueVerts.Empty(VertexCount);

	// Combine matching verts using hashed search to maintain good performance
	TMap<FVector, int32> HashedVerts;
	for (int32 a = 0; a < VertexCount; a++)
	{
		const CVecT& Position = RenderMesh.Verts[a].Position;
		FVector PositionA;
		PositionA.Set(Position[0], Position[1], Position[2]);
		const int32* FoundIndex = HashedVerts.Find(PositionA);
		if (!FoundIndex)
		{
			int32 NewIndex = UniqueVerts.Add(a);
			VertRemap[a] = NewIndex;
			HashedVerts.Add(PositionA, NewIndex);
		}
		else
		{
			VertRemap[a] = *FoundIndex;
		}
	}
}

void DetermineUVsToWeld(TArray<int32>& VertRemap, TArray<int32>& UniqueVerts, const CStaticMeshLod& RenderMesh, int32 TexCoordSourceIndex)
{
	const int32 VertexCount = RenderMesh.NumVerts;

	// Maps unreal verts to reduced list of verts
	VertRemap.Empty(VertexCount);
	VertRemap.AddUninitialized(VertexCount);

	// List of Unreal Verts to keep
	UniqueVerts.Empty(VertexCount);

	// Combine matching verts using hashed search to maintain good performance
	TMap<FVector2D, int32> HashedVerts;
	for (int32 Vertex = 0; Vertex < VertexCount; Vertex++)
	{
		const CMeshUVFloat& MeshUV = RenderMesh.GetVertexUV(Vertex, TexCoordSourceIndex);
		const FVector2D& PositionA = *(FVector2D*)&MeshUV;

		const int32* FoundIndex = HashedVerts.Find(PositionA);
		if (!FoundIndex)
		{
			int32 NewIndex = UniqueVerts.Add(Vertex);
			VertRemap[Vertex] = NewIndex;
			HashedVerts.Add(PositionA, NewIndex);
		}
		else
		{
			VertRemap[Vertex] = *FoundIndex;
		}
	}
}

FbxNode* FFbxExporter::ExportStaticMeshToFbx(const CStaticMesh* StaticMesh, int32 ExportLOD, const char* MeshName, FbxNode* FbxActor, int32 LightmapUVChannel /*= -1*/)
{
	FbxMesh* Mesh = nullptr;
	if ((ExportLOD == 0 || ExportLOD == -1) && LightmapUVChannel == -1)
	{
		if (auto mesh_ptr = FbxMeshes.Find(StaticMesh))
			Mesh = *mesh_ptr;
	}

	if (!Mesh)
	{
		Mesh = FbxMesh::Create(Scene, MeshName);

		const CStaticMeshLod& RenderMesh = StaticMesh->Lods[ExportLOD];

		// Verify the integrity of the static mesh.
		if (RenderMesh.NumVerts == 0)
		{
			return nullptr;
		}

		if (RenderMesh.Sections.Num() == 0)
		{
			return nullptr;
		}

		// Remaps an Unreal vert to final reduced vertex list
		TArray<int32> VertRemap;
		TArray<int32> UniqueVerts;

		if (ExportOptions->WeldedVertices)
		{
			// Weld verts
			DetermineVertsToWeld(VertRemap, UniqueVerts, RenderMesh);
		}
		else
		{
			// Do not weld verts
			VertRemap.AddUninitialized(RenderMesh.NumVerts);
			for (int32 i = 0; i < VertRemap.Num(); i++)
			{
				VertRemap[i] = i;
			}
			CopyArray(UniqueVerts, VertRemap);
		}

		// Create and fill in the vertex position data source.
		// The position vertices are duplicated, for some reason, retrieve only the first half vertices.
		const int32 VertexCount = VertRemap.Num();
		const int32 PolygonsCount = RenderMesh.Sections.Num();

		Mesh->InitControlPoints(UniqueVerts.Num());

		FbxVector4* ControlPoints = Mesh->GetControlPoints();
		for (int32 PosIndex = 0; PosIndex < UniqueVerts.Num(); ++PosIndex)
		{
			int32 UnrealPosIndex = UniqueVerts[PosIndex];
			CVecT& Position = RenderMesh.Verts[UnrealPosIndex].Position;
			ControlPoints[PosIndex] = FbxVector4(Position[0], -Position[1], Position[2]);
		}

		// Set the normals on Layer 0.
		FbxLayer* Layer = Mesh->GetLayer(0);
		if (Layer == nullptr)
		{
			Mesh->CreateLayer();
			Layer = Mesh->GetLayer(0);
		}

		// Build list of Indices re-used multiple times to lookup Normals, UVs, other per face vertex information
		TArray<uint32> Indices;
		for (int32 PolygonsIndex = 0; PolygonsIndex < PolygonsCount; ++PolygonsIndex)
		{
			auto RawIndices = RenderMesh.Indices.GetAccessor();
			const CMeshSection& Polygons = RenderMesh.Sections[PolygonsIndex];
			const uint32 TriangleCount = Polygons.NumFaces;
			for (uint32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
			{
				for (uint32 PointIndex = 0; PointIndex < 3; PointIndex++)
				{
					uint32 UnrealVertIndex = RawIndices(Polygons.FirstIndex + ((TriangleIndex * 3) + PointIndex));
					Indices.Add(UnrealVertIndex);
				}
			}
		}

		// Create and fill in the per-face-vertex normal data source.
		// We extract the Z-tangent and the X/Y-tangents which are also stored in the render mesh.
		FbxLayerElementNormal* LayerElementNormal = FbxLayerElementNormal::Create(Mesh, "");
		FbxLayerElementTangent* LayerElementTangent = FbxLayerElementTangent::Create(Mesh, "");
		FbxLayerElementBinormal* LayerElementBinormal = FbxLayerElementBinormal::Create(Mesh, "");

		// Set 3 NTBs per triangle instead of storing on positional control points
		LayerElementNormal->SetMappingMode(FbxLayerElement::eByPolygonVertex);
		LayerElementTangent->SetMappingMode(FbxLayerElement::eByPolygonVertex);
		LayerElementBinormal->SetMappingMode(FbxLayerElement::eByPolygonVertex);

		// Set the NTBs values for every polygon vertex.
		LayerElementNormal->SetReferenceMode(FbxLayerElement::eDirect);
		LayerElementTangent->SetReferenceMode(FbxLayerElement::eDirect);
		LayerElementBinormal->SetReferenceMode(FbxLayerElement::eDirect);

		TArray<FbxVector4> FbxNormals;
		TArray<FbxVector4> FbxTangents;
		TArray<FbxVector4> FbxBinormals;

		FbxNormals.AddUninitialized(VertexCount);
		FbxTangents.AddUninitialized(VertexCount);
		FbxBinormals.AddUninitialized(VertexCount);

		for (int32 NTBIndex = 0; NTBIndex < VertexCount; ++NTBIndex)
		{
			CVecT Normal, Tangent, Binormal;
			RenderMesh.Verts[NTBIndex].DecodeTangents(Normal, Tangent, Binormal);

			FbxVector4& FbxNormal = FbxNormals[NTBIndex];
			FbxNormal = FbxVector4(Normal[0], -Normal[1], Normal[2]);
			FbxNormal.Normalize();

			FbxVector4& FbxTangent = FbxTangents[NTBIndex];
			FbxTangent = FbxVector4(Tangent[0], -Tangent[1], Tangent[2]);
			FbxTangent.Normalize();

			FbxVector4& FbxBinormal = FbxBinormals[NTBIndex];
			FbxBinormal = FbxVector4(Binormal[0], -Binormal[1], Binormal[2]);
			FbxBinormal.Normalize();
		}

		// Add one normal per each face index (3 per triangle)
		for (int32 FbxVertIndex = 0; FbxVertIndex < Indices.Num(); FbxVertIndex++)
		{
			uint32 UnrealVertIndex = Indices[FbxVertIndex];
			LayerElementNormal->GetDirectArray().Add(FbxNormals[UnrealVertIndex]);
			LayerElementTangent->GetDirectArray().Add(FbxTangents[UnrealVertIndex]);
			LayerElementBinormal->GetDirectArray().Add(FbxBinormals[UnrealVertIndex]);
		}

		Layer->SetNormals(LayerElementNormal);
		Layer->SetTangents(LayerElementTangent);
		Layer->SetBinormals(LayerElementBinormal);

		FbxNormals.Empty();
		FbxTangents.Empty();
		FbxBinormals.Empty();

		// Create and fill in the per-face-vertex texture coordinate data source(s).
		// Create UV for Diffuse channel.
		int32 TexCoordSourceCount = (LightmapUVChannel == -1) ? RenderMesh.NumTexCoords : LightmapUVChannel + 1;
		int32 TexCoordSourceIndex = (LightmapUVChannel == -1) ? 0 : LightmapUVChannel;
		for (; TexCoordSourceIndex < TexCoordSourceCount; ++TexCoordSourceIndex)
		{
			FbxLayer* UVsLayer = (LightmapUVChannel == -1) ? Mesh->GetLayer(TexCoordSourceIndex) : Mesh->GetLayer(0);
			if (UVsLayer == nullptr)
			{
				Mesh->CreateLayer();
				UVsLayer = (LightmapUVChannel == -1) ? Mesh->GetLayer(TexCoordSourceIndex) : Mesh->GetLayer(0);
			}
			assert(UVsLayer);

			char UVChannelName[128];
			appSprintf(UVChannelName, 128, "UVmap_%d", TexCoordSourceIndex);
			if ((LightmapUVChannel >= 0) || (LightmapUVChannel == -1 && (TexCoordSourceIndex == 1/*StaticMesh->LightMapCoordinateIndex*/)))
			{
				appSprintf(UVChannelName, 128, "%s", "LightMapUV");
			}

			FbxLayerElementUV* UVDiffuseLayer = FbxLayerElementUV::Create(Mesh, UVChannelName);

			// Note: when eINDEX_TO_DIRECT is used, IndexArray must be 3xTriangle count, DirectArray can be smaller
			UVDiffuseLayer->SetMappingMode(FbxLayerElement::eByPolygonVertex);
			UVDiffuseLayer->SetReferenceMode(FbxLayerElement::eIndexToDirect);

			TArray<int32> UvsRemap;
			TArray<int32> UniqueUVs;
			if (ExportOptions->WeldedVertices)
			{
				// Weld UVs
				DetermineUVsToWeld(UvsRemap, UniqueUVs, RenderMesh, TexCoordSourceIndex);
			}
			else
			{
				// Do not weld UVs
				CopyArray(UvsRemap, VertRemap);
				CopyArray(UniqueUVs, UvsRemap);
			}

			// Create the texture coordinate data source.
			for (int32 FbxVertIndex = 0; FbxVertIndex < UniqueUVs.Num(); FbxVertIndex++)
			{
				int32 UnrealVertIndex = UniqueUVs[FbxVertIndex];
				const FVector2D& TexCoord = *(FVector2D*)&RenderMesh.GetVertexUV(UnrealVertIndex, TexCoordSourceIndex);
				UVDiffuseLayer->GetDirectArray().Add(FbxVector2(TexCoord.X, -TexCoord.Y + 1.0));
			}

			// For each face index, point to a texture uv
			UVDiffuseLayer->GetIndexArray().SetCount(Indices.Num());
			for (int32 FbxVertIndex = 0; FbxVertIndex < Indices.Num(); FbxVertIndex++)
			{
				uint32 UnrealVertIndex = Indices[FbxVertIndex];
				int32 NewVertIndex = UvsRemap[UnrealVertIndex];
				UVDiffuseLayer->GetIndexArray().SetAt(FbxVertIndex, NewVertIndex);
			}

			UVsLayer->SetUVs(UVDiffuseLayer, FbxLayerElement::eTextureDiffuse);
		}

		FbxLayerElementMaterial* MatLayer = FbxLayerElementMaterial::Create(Mesh, "");
		MatLayer->SetMappingMode(FbxLayerElement::eByPolygon);
		MatLayer->SetReferenceMode(FbxLayerElement::eIndexToDirect);
		Layer->SetMaterials(MatLayer);

		// Keep track of the number of tri's we export
		uint32 AccountedTriangles = 0;
		for (int32 PolygonsIndex = 0; PolygonsIndex < PolygonsCount; ++PolygonsIndex)
		{
			const CMeshSection& Polygons = RenderMesh.Sections[PolygonsIndex];
			auto RawIndices = RenderMesh.Indices.GetAccessor();
			UUnrealMaterial* Material = Polygons.Material;

			FbxSurfaceMaterial* FbxMaterial = Material ? this->ExportMaterial(Material) : NULL;
			if (!FbxMaterial)
			{
				FbxMaterial = CreateDefaultMaterial(PolygonsIndex);
			}
			int32 MatIndex = FbxActor->AddMaterial(FbxMaterial);

			// Determine the actual material index
			int32 ActualMatIndex = MatIndex;

			// Static meshes contain one triangle list per element.
			// [GLAFORTE] Could it occasionally contain triangle strips? How do I know?
			uint32 TriangleCount = Polygons.NumFaces;

			// Copy over the index buffer into the FBX polygons set.
			for (uint32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
			{
				Mesh->BeginPolygon(ActualMatIndex);
				for (uint32 PointIndex = 0; PointIndex < 3; PointIndex++)
				{
					uint32 OriginalUnrealVertIndex = RawIndices(Polygons.FirstIndex + ((TriangleIndex * 3) + PointIndex));
					int32 RemappedVertIndex = VertRemap[OriginalUnrealVertIndex];
					Mesh->AddPolygon(RemappedVertIndex);
				}
				Mesh->EndPolygon();
			}

			AccountedTriangles += TriangleCount;
		}

		if ((ExportLOD == 0 || ExportLOD == -1) && LightmapUVChannel == -1)
		{
			FbxMeshes.Add(StaticMesh, Mesh);
		}
	}
	else
	{
		//Materials in fbx are store in the node and not in the mesh, so even if the mesh was already export
		//we have to find and assign the mesh material.
		const CStaticMeshLod& RenderMesh = StaticMesh->Lods[ExportLOD];
		const int32 PolygonsCount = RenderMesh.Sections.Num();
		uint32 AccountedTriangles = 0;
		for (int32 PolygonsIndex = 0; PolygonsIndex < PolygonsCount; ++PolygonsIndex)
		{
			const CMeshSection& Polygons = RenderMesh.Sections[PolygonsIndex];
			auto RawIndices = RenderMesh.Indices.GetAccessor();
			UUnrealMaterial* Material = Polygons.Material;

			FbxSurfaceMaterial* FbxMaterial = Material ? this->ExportMaterial(Material) : NULL;
			if (!FbxMaterial)
			{
				FbxMaterial = CreateDefaultMaterial(PolygonsIndex);
			}
			FbxActor->AddMaterial(FbxMaterial);
		}
	}

	//Set the original meshes in case it was already existing
	FbxActor->SetNodeAttribute(Mesh);

	return FbxActor;
}

FbxSurfaceMaterial* FFbxExporter::CreateDefaultMaterial(int Index)
{
	char DefaultMaterialName[128];
	appSprintf(DefaultMaterialName, 128, "DefaultMaterial%d", Index);
	FbxSurfaceMaterial* FbxMaterial = Scene->GetMaterial(DefaultMaterialName);

	if (!FbxMaterial)
	{
		FbxMaterial = FbxSurfaceLambert::Create(Scene, DefaultMaterialName);
		((FbxSurfaceLambert*)FbxMaterial)->Diffuse.Set(FbxDouble3(0.72, 0.72, 0.72));
	}

	return FbxMaterial;
}

FbxSurfaceMaterial* FFbxExporter::ExportMaterial(UUnrealMaterial* Material)
{
	if (Scene == nullptr || Material == nullptr) return nullptr;

	// Verify that this material has not already been exported:
	if (FbxMaterials.Find(Material))
	{
		return *FbxMaterials.Find(Material);
	}

	CMaterialParams Params;
	Material->GetParams(Params);
	if (Params.IsNull() || Params.Diffuse == Material) return nullptr;	// empty/unknown material, or material itself is a texture

	// Create the Fbx material
	FbxSurfaceMaterial* FbxMaterial;

	// Set the shading model
	//if (Material->GetShadingModel() == MSM_DefaultLit)
	{
		FbxMaterial = FbxSurfacePhong::Create(Scene, Material->Name);
		//((FbxSurfacePhong*)FbxMaterial)->Specular.Set(Material->Specular));
		//((FbxSurfacePhong*)FbxMaterial)->Shininess.Set(Material->SpecularPower.Constant);
	}
	//else // if (Material->ShadingModel == MSM_Unlit)
	//{
	//	FbxMaterial = FbxSurfaceLambert::Create(Scene, Material->Name);
	//}

	//((FbxSurfaceLambert*)FbxMaterial)->TransparencyFactor.Set(Material->Opacity.Constant);

	// Fill in the profile_COMMON effect with the material information.
	//Fill the texture or constant
	if (!FillFbxTextureProperty(FbxSurfaceMaterial::sDiffuse, Params.Diffuse, FbxMaterial))
	{
		//((FbxSurfaceLambert*)FbxMaterial)->Diffuse.Set(SetMaterialComponent(Material->BaseColor, true));
	}
	if (!FillFbxTextureProperty(FbxSurfaceMaterial::sEmissive, Params.Emissive, FbxMaterial))
	{
		//((FbxSurfaceLambert*)FbxMaterial)->Emissive.Set(FbxDouble3(Params.EmissiveColor.R, Params.EmissiveColor.G, Params.EmissiveColor.B));
	}

	//Always set the ambient to zero since we dont have ambient in unreal we want to avoid default value in DCCs
	((FbxSurfaceLambert*)FbxMaterial)->Ambient.Set(FbxDouble3(0.0, 0.0, 0.0));

	//Set the Normal map only if there is a texture sampler
	FillFbxTextureProperty(FbxSurfaceMaterial::sNormalMap, Params.Normal, FbxMaterial);
	FillFbxTextureProperty(FbxSurfaceMaterial::sSpecular, Params.Specular, FbxMaterial);
	FillFbxTextureProperty(FbxSurfaceMaterial::sShininess, Params.SpecPower, FbxMaterial);
	FillFbxTextureProperty(FbxSurfaceMaterial::sReflection, Params.Cube, FbxMaterial);

	//UMaterial3* Material3 = Material->IsA("Material3") ? static_cast<UMaterial3*>(Material) : nullptr;
	//if (Material3 != nullptr && Material3->BlendMode == BLEND_Translucent)
	//{
	//	if (!FillFbxTextureProperty(FbxSurfaceMaterial::sTransparentColor, Params.Opacity, FbxMaterial))
	//	{
	//		//FbxDouble3 OpacityValue((FbxDouble)(Material->Opacity.Constant), (FbxDouble)(Material->Opacity.Constant), (FbxDouble)(Material->Opacity.Constant));
	//		//((FbxSurfaceLambert*)FbxMaterial)->TransparentColor.Set(OpacityValue);
	//	}
	//	if (!FillFbxTextureProperty(FbxSurfaceMaterial::sTransparencyFactor, Params.Opacity, FbxMaterial))
	//	{
	//		((FbxSurfaceLambert*)FbxMaterial)->TransparencyFactor.Set(Material3->OpacityMaskClipValue);
	//	}
	//}

	FbxMaterials.Add(Material, FbxMaterial);

	return FbxMaterial;
}

bool FFbxExporter::FillFbxTextureProperty(const char *PropertyName, const UUnrealMaterial* Texture, FbxSurfaceMaterial* FbxMaterial)
{
	if (Scene == nullptr)
	{
		return false;
	}

	FbxProperty FbxColorProperty = FbxMaterial->FindProperty(PropertyName);
	if (FbxColorProperty.IsValid())
	{
		if (Texture)
		{
			const char* TextureSourceFullPath = GetExportTextureName(Texture);
			//Create a fbx property
			FbxFileTexture* lTexture = FbxFileTexture::Create(Scene, "EnvSamplerTex");
			lTexture->SetFileName(TextureSourceFullPath);
			lTexture->SetTextureUse(FbxTexture::eStandard);
			lTexture->SetMappingType(FbxTexture::eUV);
			lTexture->ConnectDstProperty(FbxColorProperty);
			return true;
		}
	}
	return false;
}

void ExportStaticMeshFBX(const CStaticMesh *Mesh)
{
	UObject *OriginalMesh = Mesh->OriginalMesh;
	if (!Mesh->Lods.Num())
	{
		appNotify("Mesh %s has 0 lods", OriginalMesh->Name);
		return;
	}

	FFbxExporter* FbxExporter = FFbxExporter::GetInstance();
	FbxExporter->CreateDocument();
	FbxExporter->ExportStaticMesh(Mesh);
	const char* Filename = GetExportFileName(OriginalMesh, "%s.fbx", OriginalMesh->Name);
	FbxExporter->WriteToFile(Filename);
}