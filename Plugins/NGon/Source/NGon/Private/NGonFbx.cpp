#include "NGonFbx.h"

#include "Algo/Reverse.h"
#include "HAL/FileManager.h"
#include "MeshDescription.h"
#include "Misc/Paths.h"
#include "NGonTopology.h"
#include "StaticMeshAttributes.h"

THIRD_PARTY_INCLUDES_START
#include <fbxsdk.h>
THIRD_PARTY_INCLUDES_END

#define LOCTEXT_NAMESPACE "NGonFbx"

namespace
{
struct FDestroyFbxManager
{
    void operator()(FbxManager* Manager) const
    {
        if (Manager)
        {
            Manager->Destroy();
        }
    }
};

struct FHeatAccumulator
{
    double Sum = 0.0;
    int32 Count = 0;
};

struct FFbxCornerUV
{
    FVector2f UV = FVector2f::ZeroVector;
    int32 SeamKey = INDEX_NONE;
    bool bValid = false;
};

FText MakeFbxError(const FText& Message, FbxImporter& Importer)
{
    return FText::Format(LOCTEXT("FbxError", "{0} ({1})"),
        Message, FText::FromString(UTF8_TO_TCHAR(Importer.GetStatus().GetErrorString())));
}

bool ReadRed(
    const FbxGeometryElementVertexColor& Colors, int32 Vertex, int32 PolygonCorner, double& OutRed)
{
    int32 ColorIndex;
    switch (Colors.GetMappingMode())
    {
    case FbxGeometryElement::eByControlPoint:
        ColorIndex = Vertex;
        break;
    case FbxGeometryElement::eByPolygonVertex:
        ColorIndex = PolygonCorner;
        break;
    default:
        return false;
    }

    const auto& Indices = Colors.GetIndexArray();
    switch (Colors.GetReferenceMode())
    {
    case FbxGeometryElement::eDirect:
        break;
    case FbxGeometryElement::eIndexToDirect:
        if (ColorIndex < 0 || ColorIndex >= Indices.GetCount())
        {
            return false;
        }
        ColorIndex = Indices.GetAt(ColorIndex);
        break;
    default:
        return false;
    }

    const auto& Values = Colors.GetDirectArray();
    if (ColorIndex < 0 || ColorIndex >= Values.GetCount())
    {
        return false;
    }

    const double Red = Values.GetAt(ColorIndex).mRed;
    if (!FMath::IsFinite(Red))
    {
        return false;
    }
    OutRed = FMath::Clamp(Red, 0.0, 1.0);
    return true;
}

bool ReadCornerUV(
    const FbxGeometryElementUV& UVs,
    int32 BaseUVOffset,
    int32 Vertex,
    int32 PolygonCorner,
    FFbxCornerUV& OutCornerUV)
{
    int32 MapIndex = INDEX_NONE;
    switch (UVs.GetMappingMode())
    {
    case FbxGeometryElement::eByControlPoint:
        MapIndex = Vertex;
        break;
    case FbxGeometryElement::eByPolygonVertex:
        MapIndex = PolygonCorner;
        break;
    default:
        return false;
    }

    int32 DirectIndex = MapIndex;
    const auto& Indices = UVs.GetIndexArray();
    switch (UVs.GetReferenceMode())
    {
    case FbxGeometryElement::eDirect:
        break;
    case FbxGeometryElement::eIndexToDirect:
        if (MapIndex < 0 || MapIndex >= Indices.GetCount())
        {
            return false;
        }
        DirectIndex = Indices.GetAt(MapIndex);
        break;
    default:
        return false;
    }

    const auto& Values = UVs.GetDirectArray();
    if (DirectIndex < 0 || DirectIndex >= Values.GetCount())
    {
        return false;
    }

    const FbxVector2 Value = Values.GetAt(DirectIndex);
    if (!FMath::IsFinite(Value[0]) || !FMath::IsFinite(Value[1]))
    {
        return false;
    }

    OutCornerUV.UV = FVector2f(static_cast<float>(Value[0]), static_cast<float>(Value[1]));
    OutCornerUV.SeamKey = (UVs.GetReferenceMode() == FbxGeometryElement::eIndexToDirect)
        ? (BaseUVOffset + DirectIndex)
        : INDEX_NONE;
    OutCornerUV.bValid = true;
    return true;
}

bool AppendMesh(
    FbxNode& Node,
    FbxMesh& Mesh,
    double ToCentimeters,
    int32& InOutUVKeyOffset,
    TArray<FVector>& Positions,
    TArray<TArray<int32>>& Faces,
    TArray<TArray<FFbxCornerUV>>& FaceUVs,
    TArray<float>& RedSamples,
    FText& Error)
{
    FbxAMatrix GeometryTransform;
    GeometryTransform.SetT(Node.GetGeometricTranslation(FbxNode::eSourcePivot));
    GeometryTransform.SetR(Node.GetGeometricRotation(FbxNode::eSourcePivot));
    GeometryTransform.SetS(Node.GetGeometricScaling(FbxNode::eSourcePivot));
    const FbxAMatrix World = Node.EvaluateGlobalTransform() * GeometryTransform;
    const FVector RowX(World.Get(0, 0), World.Get(0, 1), World.Get(0, 2));
    const FVector RowY(World.Get(1, 0), World.Get(1, 1), World.Get(1, 2));
    const FVector RowZ(World.Get(2, 0), World.Get(2, 1), World.Get(2, 2));
    // The Y reflection changes winding. A mirrored FBX node changes it back.
    const bool bReverseWinding = FVector::DotProduct(RowX, FVector::CrossProduct(RowY, RowZ)) >= 0.0;

    const int32 BaseVertex = Positions.Num();
    const int32 PointCount = Mesh.GetControlPointsCount();
    const FbxVector4* Points = Mesh.GetControlPoints();
    if (!Points || PointCount < 3)
    {
        Error = LOCTEXT("MissingControlPoints", "An FBX mesh has faces but fewer than three control points.");
        return false;
    }

    Positions.Reserve(BaseVertex + PointCount);
    for (int32 Vertex = 0; Vertex < PointCount; ++Vertex)
    {
        const FbxVector4 Point = World.MultT(Points[Vertex]);
        Positions.Add(FVector(Point[0], -Point[1], Point[2]) * ToCentimeters);
    }

    const FbxGeometryElementVertexColor* Colors = Mesh.GetElementVertexColor(0);
    TArray<FHeatAccumulator> Samples;
    if (Colors)
    {
        Samples.SetNum(PointCount);
    }

    const FbxGeometryElementUV* UVElement = Mesh.GetElementUV(0);
    const int32 BaseUVOffset = InOutUVKeyOffset;
    if (UVElement)
    {
        InOutUVKeyOffset += FMath::Max(UVElement->GetDirectArray().GetCount(), 1);
    }

    const int32 FaceCount = Mesh.GetPolygonCount();
    Faces.Reserve(Faces.Num() + FaceCount);
    FaceUVs.Reserve(FaceUVs.Num() + FaceCount);
    for (int32 Polygon = 0; Polygon < FaceCount; ++Polygon)
    {
        const int32 Corners = Mesh.GetPolygonSize(Polygon);
        const int32 FirstCorner = Mesh.GetPolygonVertexIndex(Polygon);
        if (Corners < 3 || FirstCorner < 0 || FirstCorner > Mesh.GetPolygonVertexCount() - Corners
            || Mesh.GetPolyHoleInfo(Polygon))
        {
            Error = FText::Format(LOCTEXT("BadFbxPolygon", "{0}, face {1} needs one valid perimeter."),
                FText::FromString(UTF8_TO_TCHAR(Node.GetName())), FText::AsNumber(Polygon));
            return false;
        }

        TArray<int32>& Face = Faces.AddDefaulted_GetRef();
        TArray<FFbxCornerUV>& CornerUVList = FaceUVs.AddDefaulted_GetRef();
        Face.Reserve(Corners);
        CornerUVList.Reserve(Corners);
        for (int32 Corner = 0; Corner < Corners; ++Corner)
        {
            const int32 Vertex = Mesh.GetPolygonVertex(Polygon, Corner);
            if (Vertex < 0 || Vertex >= PointCount)
            {
                Error = FText::Format(LOCTEXT("BadControlPoint", "{0}, face {1} has an invalid control point index."),
                    FText::FromString(UTF8_TO_TCHAR(Node.GetName())), FText::AsNumber(Polygon));
                return false;
            }

            Face.Add(BaseVertex + Vertex);

            FFbxCornerUV ParsedUV;
            if (UVElement)
            {
                ReadCornerUV(*UVElement, BaseUVOffset, Vertex, FirstCorner + Corner, ParsedUV);
            }
            CornerUVList.Add(ParsedUV);

            double Red = 0.0;
            if (Colors && ReadRed(*Colors, Vertex, FirstCorner + Corner, Red))
            {
                Samples[Vertex].Sum += Red;
                ++Samples[Vertex].Count;
            }
        }
        if (bReverseWinding)
        {
            Algo::Reverse(Face);
            Algo::Reverse(CornerUVList);
        }
    }

    RedSamples.SetNum(BaseVertex + PointCount);
    for (int32 Vertex = 0; Vertex < PointCount; ++Vertex)
    {
        RedSamples[BaseVertex + Vertex] = Colors && Samples[Vertex].Count > 0
            ? static_cast<float>(Samples[Vertex].Sum / Samples[Vertex].Count)
            : -1.0f;
    }
    return true;
}

FTransform FbxMatrixToUnrealTransform(const FbxAMatrix& Matrix, double ToCentimeters)
{
    const FbxVector4 T = Matrix.GetT();
    const FbxQuaternion Q = Matrix.GetQ();
    const FbxVector4 S = Matrix.GetS();

    const FVector Location(T[0] * ToCentimeters, -T[1] * ToCentimeters, T[2] * ToCentimeters);
    FQuat Rotation(-Q[0], Q[1], -Q[2], Q[3]);
    if (!Rotation.ContainsNaN() && Rotation.SizeSquared() > SMALL_NUMBER)
    {
        Rotation.Normalize();
    }
    else
    {
        Rotation = FQuat::Identity;
    }
    const FVector Scale(
        FMath::IsNearlyZero(S[0]) ? 1.0 : S[0],
        FMath::IsNearlyZero(S[1]) ? 1.0 : S[1],
        FMath::IsNearlyZero(S[2]) ? 1.0 : S[2]);
    return FTransform(Rotation, Location, Scale);
}

void CollectSkeletonNodesRecursive(
    FbxNode* Node,
    const TSet<FbxNode*>& BoneNodeSet,
    int32 NearestBoneParentIndex,
    double ToCentimeters,
    TArray<NGon::FNGonSceneBoneInit>& OutBones,
    TMap<FbxNode*, int32>& OutNodeToBoneIndex)
{
    if (!Node)
    {
        return;
    }

    int32 CurrentBoneIndex = NearestBoneParentIndex;
    if (BoneNodeSet.Contains(Node))
    {
        const int32 NewIndex = OutBones.Num();
        NGon::FNGonSceneBoneInit& Bone = OutBones.AddDefaulted_GetRef();
        Bone.BoneIndex = NewIndex;
        Bone.ParentIndex = NearestBoneParentIndex;
        const char* RawName = Node->GetName();
        Bone.Name = (RawName && RawName[0] != '\0')
            ? FName(UTF8_TO_TCHAR(RawName))
            : FName(*FString::Printf(TEXT("Bone_%d"), NewIndex));

        const FbxAMatrix GlobalMatrix = Node->EvaluateGlobalTransform(FBXSDK_TIME_INFINITE);
        Bone.RefComponentTransform = FbxMatrixToUnrealTransform(GlobalMatrix, ToCentimeters);
        if (NearestBoneParentIndex >= 0 && OutBones.IsValidIndex(NearestBoneParentIndex))
        {
            Bone.RefLocalTransform = Bone.RefComponentTransform.GetRelativeTransform(
                OutBones[NearestBoneParentIndex].RefComponentTransform);
        }
        else
        {
            Bone.RefLocalTransform = Bone.RefComponentTransform;
        }

        OutNodeToBoneIndex.Add(Node, NewIndex);
        CurrentBoneIndex = NewIndex;
    }

    const int32 ChildCount = Node->GetChildCount();
    for (int32 ChildIdx = 0; ChildIdx < ChildCount; ++ChildIdx)
    {
        CollectSkeletonNodesRecursive(
            Node->GetChild(ChildIdx),
            BoneNodeSet,
            CurrentBoneIndex,
            ToCentimeters,
            OutBones,
            OutNodeToBoneIndex);
    }
}

void ExtractMeshSkinAndMorphs(
    FbxMesh& Mesh,
    int32 BaseVertex,
    int32 PointCount,
    const TMap<FbxNode*, int32>& NodeToBoneIndex,
    TArray<TArray<NGon::FNGonVertexBoneWeight>>& SourceVertexSkinWeights,
    TArray<NGon::FNGonFbxMorphInfo>& OutMorphs)
{
    if (SourceVertexSkinWeights.Num() < BaseVertex + PointCount)
    {
        SourceVertexSkinWeights.SetNum(BaseVertex + PointCount);
    }

    const int32 SkinCount = Mesh.GetDeformerCount(FbxDeformer::eSkin);
    for (int32 SkinIdx = 0; SkinIdx < SkinCount; ++SkinIdx)
    {
        FbxSkin* Skin = static_cast<FbxSkin*>(Mesh.GetDeformer(SkinIdx, FbxDeformer::eSkin));
        if (!Skin)
        {
            continue;
        }
        const int32 ClusterCount = Skin->GetClusterCount();
        for (int32 ClusterIdx = 0; ClusterIdx < ClusterCount; ++ClusterIdx)
        {
            FbxCluster* Cluster = Skin->GetCluster(ClusterIdx);
            if (!Cluster)
            {
                continue;
            }
            FbxNode* LinkNode = Cluster->GetLink();
            const int32* BoneIndexPtr = LinkNode ? NodeToBoneIndex.Find(LinkNode) : nullptr;
            if (!BoneIndexPtr)
            {
                continue;
            }
            const int32 BoneIndex = *BoneIndexPtr;
            const int32 InfluenceCount = Cluster->GetControlPointIndicesCount();
            const int32* Indices = Cluster->GetControlPointIndices();
            const double* Weights = Cluster->GetControlPointWeights();
            if (!Indices || !Weights || InfluenceCount <= 0)
            {
                continue;
            }
            for (int32 i = 0; i < InfluenceCount; ++i)
            {
                const int32 LocalVert = Indices[i];
                const float Weight = static_cast<float>(Weights[i]);
                if (LocalVert < 0 || LocalVert >= PointCount || !FMath::IsFinite(Weight) || Weight <= 1.e-5f)
                {
                    continue;
                }
                TArray<NGon::FNGonVertexBoneWeight>& VertList = SourceVertexSkinWeights[BaseVertex + LocalVert];
                bool bMerged = false;
                for (NGon::FNGonVertexBoneWeight& Existing : VertList)
                {
                    if (Existing.BoneIndex == BoneIndex)
                    {
                        Existing.Weight += Weight;
                        bMerged = true;
                        break;
                    }
                }
                if (!bMerged)
                {
                    VertList.Add({ BoneIndex, Weight });
                }
            }
        }
    }

    const int32 BlendShapeCount = Mesh.GetDeformerCount(FbxDeformer::eBlendShape);
    for (int32 BsIdx = 0; BsIdx < BlendShapeCount; ++BsIdx)
    {
        FbxBlendShape* BlendShape = static_cast<FbxBlendShape*>(Mesh.GetDeformer(BsIdx, FbxDeformer::eBlendShape));
        if (!BlendShape)
        {
            continue;
        }
        const int32 ChannelCount = BlendShape->GetBlendShapeChannelCount();
        for (int32 ChIdx = 0; ChIdx < ChannelCount; ++ChIdx)
        {
            FbxBlendShapeChannel* Channel = BlendShape->GetBlendShapeChannel(ChIdx);
            if (!Channel)
            {
                continue;
            }
            NGon::FNGonFbxMorphInfo Info;
            const char* RawName = Channel->GetName();
            Info.Name = (RawName && RawName[0] != '\0')
                ? FName(UTF8_TO_TCHAR(RawName))
                : FName(*FString::Printf(TEXT("Morph_%d"), OutMorphs.Num()));
            if (Channel->GetTargetShapeCount() > 0)
            {
                if (FbxShape* Shape = Channel->GetTargetShape(0))
                {
                    Info.VertexDeltaCount = Shape->GetControlPointsCount();
                }
            }
            OutMorphs.Add(Info);
        }
    }
}

bool BuildFbxMeshDescription(
    const TArray<FVector>& Positions,
    const TArray<TArray<int32>>& Faces,
    const TArray<TArray<FFbxCornerUV>>& FaceUVs,
    FMeshDescription& Out,
    FText& Error)
{
    if (Positions.Num() < 3 || Faces.IsEmpty())
    {
        Error = LOCTEXT("EmptyPolygonData", "The polygon file needs at least three positions and one face.");
        return false;
    }

    FStaticMeshAttributes Attributes(Out);
    Attributes.Register();
    Out.SetNumUVChannels(1);
    auto VertexPositions = Attributes.GetVertexPositions();
    auto UVs = Attributes.GetVertexInstanceUVs();
    UVs.SetNumChannels(1);
    auto SharedUVs = Attributes.GetUVCoordinates(0);
    auto Normals = Attributes.GetVertexInstanceNormals();

    TArray<FVertexID> VertexIds;
    VertexIds.Reserve(Positions.Num());
    for (const FVector& Position : Positions)
    {
        if (Position.ContainsNaN() || Position.GetAbsMax() > TNumericLimits<float>::Max())
        {
            Error = LOCTEXT("InvalidPosition", "The mesh contains an invalid vertex position.");
            return false;
        }
        const FVertexID Vertex = Out.CreateVertex();
        VertexPositions[Vertex] = FVector3f(Position);
        VertexIds.Add(Vertex);
    }

    const FPolygonGroupID PolygonGroup = Out.CreatePolygonGroup();
    // Map either authored FBX direct UV index or quantized (VertexIndex, U, V) to shared FUVID
    // so welded UV islands share FUVIDs while authored UV seams stay split.
    TMap<int32, FUVID> SeamKeyToUVID;
    TMap<uint64, FUVID> QuantizedVertexUVToUVID;

    for (int32 FaceIndex = 0; FaceIndex < Faces.Num(); ++FaceIndex)
    {
        const TArray<int32>& Face = Faces[FaceIndex];
        const TArray<FFbxCornerUV>* CornerUVs = FaceUVs.IsValidIndex(FaceIndex) ? &FaceUVs[FaceIndex] : nullptr;

        TSet<int32> Seen;
        TArray<FVertexInstanceID> Instances;
        Instances.Reserve(Face.Num());
        for (const int32 VertexIndex : Face)
        {
            if (!VertexIds.IsValidIndex(VertexIndex) || Seen.Contains(VertexIndex))
            {
                Error = FText::Format(
                    LOCTEXT("InvalidFaceVertex", "Face {0} contains an invalid or repeated vertex index."),
                    FText::AsNumber(FaceIndex));
                return false;
            }
            Seen.Add(VertexIndex);
        }

        if (Face.Num() < 3)
        {
            Error = FText::Format(
                LOCTEXT("ShortFace", "Face {0} needs at least three vertices."),
                FText::AsNumber(FaceIndex));
            return false;
        }

        const FVector& Origin = Positions[Face[0]];
        FVector Normal = FVector::ZeroVector;
        double ScaleSquared = 0.0;
        for (int32 Corner = 1; Corner < Face.Num(); ++Corner)
        {
            const FVector A = Positions[Face[Corner]] - Origin;
            const FVector B = Positions[Face[(Corner + 1) % Face.Num()]] - Origin;
            Normal += FVector::CrossProduct(A, B);
            ScaleSquared = FMath::Max(ScaleSquared, A.SizeSquared());
        }

        const double Area = Normal.Size();
        if (!FMath::IsFinite(Area) || Area <= ScaleSquared * 1.e-12)
        {
            Error = FText::Format(
                LOCTEXT("DegenerateFace", "Face {0} has no usable area."),
                FText::AsNumber(FaceIndex));
            return false;
        }
        Normal /= Area;

        TMap<FVertexInstanceID, FUVID> UVForInstance;
        for (int32 Corner = 0; Corner < Face.Num(); ++Corner)
        {
            const int32 VertexIndex = Face[Corner];
            const FVertexInstanceID Instance = Out.CreateVertexInstance(VertexIds[VertexIndex]);
            Instances.Add(Instance);
            Normals[Instance] = FVector3f(Normal);

            FVector2f UVValue;
            int32 SeamKey = INDEX_NONE;
            if (CornerUVs && CornerUVs->IsValidIndex(Corner) && (*CornerUVs)[Corner].bValid)
            {
                UVValue = (*CornerUVs)[Corner].UV;
                SeamKey = (*CornerUVs)[Corner].SeamKey;
            }
            else
            {
                const FVector& Position = Positions[VertexIndex];
                UVValue = FVector2f(
                    static_cast<float>(Position.X / 100.0),
                    static_cast<float>(Position.Y / 100.0));
            }

            UVs.Set(Instance, 0, UVValue);

            FUVID UvId(INDEX_NONE);
            if (SeamKey != INDEX_NONE)
            {
                if (const FUVID* Existing = SeamKeyToUVID.Find(SeamKey))
                {
                    UvId = *Existing;
                }
                else
                {
                    UvId = Out.CreateUV(0);
                    SharedUVs[UvId] = UVValue;
                    SeamKeyToUVID.Add(SeamKey, UvId);
                }
            }
            else
            {
                const uint32 QuantU = static_cast<uint32>(FMath::RoundToInt(UVValue.X * 100000.0f));
                const uint32 QuantV = static_cast<uint32>(FMath::RoundToInt(UVValue.Y * 100000.0f));
                const uint64 HashKey = (static_cast<uint64>(static_cast<uint32>(VertexIndex)) << 32)
                    ^ (static_cast<uint64>(QuantU) * 2654435761ULL)
                    ^ static_cast<uint64>(QuantV);
                if (const FUVID* Existing = QuantizedVertexUVToUVID.Find(HashKey))
                {
                    UvId = *Existing;
                }
                else
                {
                    UvId = Out.CreateUV(0);
                    SharedUVs[UvId] = UVValue;
                    QuantizedVertexUVToUVID.Add(HashKey, UvId);
                }
            }

            UVForInstance.Add(Instance, UvId);
        }

        const FPolygonID Polygon = Out.CreatePolygon(PolygonGroup, Instances);
        const auto Triangles = Out.GetPolygonTriangles(Polygon);
        if (Triangles.Num() != Instances.Num() - 2)
        {
            Error = FText::Format(
                LOCTEXT("TriangulationFailed", "Face {0} could not be triangulated as a single polygon."),
                FText::AsNumber(FaceIndex));
            return false;
        }
        for (const FTriangleID Triangle : Triangles)
        {
            const auto Corners = Out.GetTriangleVertexInstances(Triangle);
            const FUVID TriangleUVs[] = {
                UVForInstance.FindChecked(Corners[0]),
                UVForInstance.FindChecked(Corners[1]),
                UVForInstance.FindChecked(Corners[2])
            };
            Out.SetTriangleUVIndices(Triangle, MakeArrayView(TriangleUVs), 0);
        }
    }

    return true;
}
}

namespace
{
bool LoadFbxInternal(
    const FString& Filename,
    FNGonMeshData& Out,
    NGon::FNGonFbxSkeletonData* OutSkeleton,
    FText& Error)
{
    if (OutSkeleton)
    {
        OutSkeleton->Bones.Reset();
        OutSkeleton->VertexSkinWeights.Reset();
        OutSkeleton->MorphTargets.Reset();
    }

    const FString Path = FPaths::ConvertRelativePathToFull(Filename);
    if (!IFileManager::Get().FileExists(*Path))
    {
        Error = LOCTEXT("FbxFileMissing", "The FBX file could not be found.");
        return false;
    }

    TUniquePtr<FbxManager, FDestroyFbxManager> Manager(FbxManager::Create());
    if (!Manager)
    {
        Error = LOCTEXT("NoFbxManager", "The FBX SDK could not initialize.");
        return false;
    }

    FbxIOSettings* Settings = FbxIOSettings::Create(Manager.Get(), IOSROOT);
    FbxImporter* Importer = FbxImporter::Create(Manager.Get(), "");
    FbxScene* Scene = FbxScene::Create(Manager.Get(), "NGonPreview");
    if (!Settings || !Importer || !Scene)
    {
        Error = LOCTEXT("FbxSetupFailed", "The FBX SDK could not create an import scene.");
        return false;
    }

    // Configure IOSettings BEFORE Importer->Initialize so FBX SDK readers see all enabled groups
    Settings->SetBoolProp(IMP_FBX_MATERIAL, false);
    Settings->SetBoolProp(IMP_FBX_TEXTURE, false);
    Settings->SetBoolProp(IMP_FBX_ANIMATION, OutSkeleton != nullptr);
    Settings->SetBoolProp(IMP_FBX_EXTRACT_EMBEDDED_DATA, false);
    Settings->SetBoolProp(IMP_FBX_GLOBAL_SETTINGS, true);
    Settings->SetBoolProp(IMP_FBX_MODEL, true);
    Settings->SetBoolProp(IMP_FBX_VERTEXCOLOR, true);
    Settings->SetBoolProp(IMP_FBX_LINK, true);
    Settings->SetBoolProp(IMP_FBX_SHAPE, true);
    Manager->SetIOSettings(Settings);

    if (!Importer->Initialize(TCHAR_TO_UTF8(*Path), -1, Settings))
    {
        Error = MakeFbxError(LOCTEXT("FbxOpenError", "The SDK could not open the FBX."), *Importer);
        return false;
    }
    if (!Importer->IsFBX())
    {
        Error = LOCTEXT("WrongFormat", "The selected file is not an FBX.");
        return false;
    }
    if (!Importer->Import(Scene))
    {
        Error = MakeFbxError(LOCTEXT("FbxReadError", "The SDK could not read the FBX scene."), *Importer);
        return false;
    }
    Importer->Destroy();
    Importer = nullptr;

    FbxAxisSystem::Max.DeepConvertScene(Scene);
    const double ToCentimeters = Scene->GetGlobalSettings().GetSystemUnit().GetScaleFactor();
    if (!FMath::IsFinite(ToCentimeters) || ToCentimeters <= 0.0)
    {
        Error = LOCTEXT("FbxBadUnits", "The FBX declares an invalid scene unit.");
        return false;
    }

    // Pass 1 (optional): discover all skeleton nodes from eSkeleton attributes, scene FbxSkeleton/FbxSkin objects, and bind poses
    TMap<FbxNode*, int32> NodeToBoneIndex;
    TArray<TArray<NGon::FNGonVertexBoneWeight>> SourceVertexSkinWeights;
    if (OutSkeleton && Scene->GetRootNode())
    {
        FbxNode* SceneRoot = Scene->GetRootNode();
        TSet<FbxNode*> BoneNodeSet;
        int32 TotalNodesScanned = 0;

        // 1a. Scene-level FbxSkeleton objects
        const int32 SceneSkelCount = Scene->GetSrcObjectCount<FbxSkeleton>();
        for (int32 i = 0; i < SceneSkelCount; ++i)
        {
            if (FbxSkeleton* SkelAttr = Scene->GetSrcObject<FbxSkeleton>(i))
            {
                if (FbxNode* SkelNode = SkelAttr->GetNode())
                {
                    BoneNodeSet.Add(SkelNode);
                }
            }
        }

        // 1b. Scene-level FbxSkin objects (catches all cluster links regardless of mesh nesting)
        const int32 SceneSkinCount = Scene->GetSrcObjectCount<FbxSkin>();
        for (int32 i = 0; i < SceneSkinCount; ++i)
        {
            if (FbxSkin* Skin = Scene->GetSrcObject<FbxSkin>(i))
            {
                const int32 ClusterCount = Skin->GetClusterCount();
                for (int32 c = 0; c < ClusterCount; ++c)
                {
                    if (FbxCluster* Cluster = Skin->GetCluster(c))
                    {
                        if (FbxNode* LinkNode = Cluster->GetLink())
                        {
                            BoneNodeSet.Add(LinkNode);
                        }
                    }
                }
            }
        }

        // 1c. Bind poses (catches non-eSkeleton joints like eNull root/armature nodes in UE/Maya/Blender exports)
        const int32 PoseCount = Scene->GetPoseCount();
        for (int32 p = 0; p < PoseCount; ++p)
        {
            if (FbxPose* Pose = Scene->GetPose(p); Pose && Pose->IsBindPose())
            {
                const int32 PoseNodeCount = Pose->GetCount();
                for (int32 n = 0; n < PoseNodeCount; ++n)
                {
                    FbxNode* PoseNode = Pose->GetNode(n);
                    if (PoseNode && PoseNode != SceneRoot && !PoseNode->GetMesh() && !PoseNode->GetCamera() && !PoseNode->GetLight())
                    {
                        BoneNodeSet.Add(PoseNode);
                    }
                }
            }
        }

        // 1d. Full tree walk: check GetSkeleton(), all node attributes, and mesh skin deformers
        TArray<FbxNode*> ScanStack;
        ScanStack.Add(SceneRoot);
        while (!ScanStack.IsEmpty())
        {
            FbxNode* Node = ScanStack.Pop(EAllowShrinking::No);
            if (!Node)
            {
                continue;
            }
            ++TotalNodesScanned;

            if (Node->GetSkeleton() != nullptr)
            {
                BoneNodeSet.Add(Node);
            }
            else
            {
                const int32 AttrCount = Node->GetNodeAttributeCount();
                for (int32 a = 0; a < AttrCount; ++a)
                {
                    if (FbxNodeAttribute* Attr = Node->GetNodeAttributeByIndex(a))
                    {
                        if (Attr->GetAttributeType() == FbxNodeAttribute::eSkeleton)
                        {
                            BoneNodeSet.Add(Node);
                            break;
                        }
                    }
                }
            }

            if (FbxMesh* Mesh = Node->GetMesh())
            {
                const int32 SkinCount = Mesh->GetDeformerCount(FbxDeformer::eSkin);
                for (int32 SkinIdx = 0; SkinIdx < SkinCount; ++SkinIdx)
                {
                    if (FbxSkin* Skin = static_cast<FbxSkin*>(Mesh->GetDeformer(SkinIdx, FbxDeformer::eSkin)))
                    {
                        const int32 ClusterCount = Skin->GetClusterCount();
                        for (int32 ClusterIdx = 0; ClusterIdx < ClusterCount; ++ClusterIdx)
                        {
                            if (FbxCluster* Cluster = Skin->GetCluster(ClusterIdx))
                            {
                                if (FbxNode* LinkNode = Cluster->GetLink())
                                {
                                    BoneNodeSet.Add(LinkNode);
                                }
                            }
                        }
                    }
                }
            }

            for (int32 Child = Node->GetChildCount() - 1; Child >= 0; --Child)
            {
                ScanStack.Add(Node->GetChild(Child));
            }
        }

        // 1e. Include immediate non-mesh eNull parent of any root bone (e.g. UE5 exported "root" null node)
        TArray<FbxNode*> ExtraParentBones;
        for (FbxNode* BoneNode : BoneNodeSet)
        {
            FbxNode* Parent = BoneNode ? BoneNode->GetParent() : nullptr;
            if (Parent && Parent != SceneRoot && !BoneNodeSet.Contains(Parent)
                && !Parent->GetMesh() && !Parent->GetCamera() && !Parent->GetLight())
            {
                FbxNodeAttribute* ParentAttr = Parent->GetNodeAttribute();
                if (!ParentAttr || ParentAttr->GetAttributeType() == FbxNodeAttribute::eNull)
                {
                    const FString ParentName = UTF8_TO_TCHAR(Parent->GetName());
                    if (ParentName.Contains(TEXT("root"), ESearchCase::IgnoreCase)
                        || ParentName.Contains(TEXT("pelvis"), ESearchCase::IgnoreCase)
                        || ParentName.Contains(TEXT("armature"), ESearchCase::IgnoreCase)
                        || ParentName.Contains(TEXT("skeleton"), ESearchCase::IgnoreCase))
                    {
                        ExtraParentBones.Add(Parent);
                    }
                }
            }
        }
        for (FbxNode* Extra : ExtraParentBones)
        {
            BoneNodeSet.Add(Extra);
        }

        // 1f. Fallback: if no eSkeleton/FbxSkin/BindPose nodes were found, detect nested eNull rig hierarchies (e.g. Root -> joints)
        if (BoneNodeSet.IsEmpty())
        {
            TFunction<void(FbxNode*, TArray<FbxNode*>&)> GatherNonMeshSubtree = [&](FbxNode* Cur, TArray<FbxNode*>& OutNodes)
            {
                if (!Cur || Cur->GetMesh() || Cur->GetCamera() || Cur->GetLight())
                {
                    return;
                }
                OutNodes.Add(Cur);
                for (int32 c = 0; c < Cur->GetChildCount(); ++c)
                {
                    GatherNonMeshSubtree(Cur->GetChild(c), OutNodes);
                }
            };
            for (int32 c = 0; c < SceneRoot->GetChildCount(); ++c)
            {
                FbxNode* Top = SceneRoot->GetChild(c);
                if (!Top || Top->GetMesh() || Top->GetCamera() || Top->GetLight())
                {
                    continue;
                }
                TArray<FbxNode*> Subtree;
                GatherNonMeshSubtree(Top, Subtree);
                if (Subtree.Num() >= 3)
                {
                    for (FbxNode* N : Subtree)
                    {
                        BoneNodeSet.Add(N);
                    }
                }
            }
        }

        if (!BoneNodeSet.IsEmpty())
        {
            CollectSkeletonNodesRecursive(
                SceneRoot,
                BoneNodeSet,
                INDEX_NONE,
                ToCentimeters,
                OutSkeleton->Bones,
                NodeToBoneIndex);
        }

        UE_LOG(LogTemp, Warning, TEXT("[NGonFbx] '%s': Nodes=%d, SceneSkeletons=%d, SceneSkins=%d, BindPoses=%d, ExtractedBones=%d"),
            *FPaths::GetCleanFilename(Path), TotalNodesScanned, SceneSkelCount, SceneSkinCount, PoseCount, OutSkeleton->Bones.Num());
        if (OutSkeleton->Bones.IsEmpty())
        {
            FString SampleNodes;
            for (int32 c = 0; c < FMath::Min(SceneRoot->GetChildCount(), 10); ++c)
            {
                FbxNode* Child = SceneRoot->GetChild(c);
                const int32 AttrType = (Child && Child->GetNodeAttribute()) ? static_cast<int32>(Child->GetNodeAttribute()->GetAttributeType()) : -1;
                SampleNodes += FString::Printf(TEXT("%s(attr=%d,ch=%d) "), Child ? UTF8_TO_TCHAR(Child->GetName()) : TEXT("null"), AttrType, Child ? Child->GetChildCount() : 0);
            }
            UE_LOG(LogTemp, Warning, TEXT("[NGonFbx] Root children: %s"), *SampleNodes);
        }
    }

    TArray<FVector> Positions;
    TArray<TArray<int32>> Faces;
    TArray<TArray<FFbxCornerUV>> FaceUVs;
    TArray<float> RedSamples;
    TArray<FNGonSubMeshInfo> FbxSubMeshes;
    int32 UVKeyOffset = 0;
    TArray<FbxNode*> Pending;
    if (FbxNode* Root = Scene->GetRootNode())
    {
        Pending.Add(Root);
    }
    while (!Pending.IsEmpty())
    {
        FbxNode* Node = Pending.Pop(EAllowShrinking::No);
        if (FbxMesh* Mesh = Node->GetMesh(); Mesh && Mesh->GetPolygonCount() > 0)
        {
            const int32 BaseVertex = Positions.Num();
            const int32 BaseFace = Faces.Num();
            const int32 PointCount = Mesh->GetControlPointsCount();
            if (!AppendMesh(*Node, *Mesh, ToCentimeters, UVKeyOffset, Positions, Faces, FaceUVs, RedSamples, Error))
            {
                return false;
            }
            const int32 AddedFaces = Faces.Num() - BaseFace;
            if (AddedFaces > 0)
            {
                FNGonSubMeshInfo& Sub = FbxSubMeshes.AddDefaulted_GetRef();
                Sub.SubMeshIndex = FbxSubMeshes.Num() - 1;
                const char* RawNodeName = Node->GetName();
                Sub.Name = (RawNodeName && RawNodeName[0] != '\0')
                    ? FName(UTF8_TO_TCHAR(RawNodeName))
                    : FName(*FString::Printf(TEXT("Mesh_%d"), Sub.SubMeshIndex));
                if (FbxNode* ParentNode = Node->GetParent(); ParentNode && ParentNode != Scene->GetRootNode())
                {
                    const char* RawParent = ParentNode->GetName();
                    if (RawParent && RawParent[0] != '\0')
                    {
                        Sub.ParentName = FName(UTF8_TO_TCHAR(RawParent));
                    }
                }
                Sub.FirstFaceId = BaseFace;
                Sub.FaceCount = AddedFaces;
                Sub.FirstVertexId = BaseVertex;
                Sub.VertexCount = PointCount;
                Sub.bVisible = true;
            }
            if (OutSkeleton)
            {
                ExtractMeshSkinAndMorphs(
                    *Mesh,
                    BaseVertex,
                    PointCount,
                    NodeToBoneIndex,
                    SourceVertexSkinWeights,
                    OutSkeleton->MorphTargets);
            }
        }
        for (int32 Child = Node->GetChildCount() - 1; Child >= 0; --Child)
        {
            Pending.Add(Node->GetChild(Child));
        }
    }

    // Release FBX SDK scene and manager as soon as mesh data is extracted;
    // FDestroyFbxManager safely guards against nullptr when ~TUniquePtr runs.
    Manager.Reset();

    if (Faces.IsEmpty())
    {
        Error = LOCTEXT("FbxNoMesh", "The FBX contains no polygon mesh faces.");
        return false;
    }

    FMeshDescription SourceMesh;
    FNGonMeshData Result;
    if (!BuildFbxMeshDescription(Positions, Faces, FaceUVs, SourceMesh, Error)
        || !NGon::ConvertMeshDescription(SourceMesh, Result, Error))
    {
        return false;
    }

    Result.ImportedHeatSamples.Reserve(Result.Topology.SourceVertexIds.Num());
    for (const int32 SourceVertex : Result.Topology.SourceVertexIds)
    {
        Result.ImportedHeatSamples.Add(RedSamples.IsValidIndex(SourceVertex)
            ? RedSamples[SourceVertex]
            : -1.0f);
    }

    if (OutSkeleton && !OutSkeleton->Bones.IsEmpty())
    {
        const int32 NumLogical = Result.Topology.SourceVertexIds.Num();
        OutSkeleton->VertexSkinWeights.SetNum(NumLogical);
        for (int32 LogicalIdx = 0; LogicalIdx < NumLogical; ++LogicalIdx)
        {
            const int32 SourceVertex = Result.Topology.SourceVertexIds[LogicalIdx];
            if (SourceVertexSkinWeights.IsValidIndex(SourceVertex) && !SourceVertexSkinWeights[SourceVertex].IsEmpty())
            {
                TArray<NGon::FNGonVertexBoneWeight>& Dst = OutSkeleton->VertexSkinWeights[LogicalIdx];
                Dst = SourceVertexSkinWeights[SourceVertex];
                float Sum = 0.0f;
                for (const NGon::FNGonVertexBoneWeight& BW : Dst)
                {
                    Sum += BW.Weight;
                }
                if (Sum > SMALL_NUMBER)
                {
                    for (NGon::FNGonVertexBoneWeight& BW : Dst)
                    {
                        BW.Weight /= Sum;
                    }
                }
            }
        }
    }

    if (!FbxSubMeshes.IsEmpty())
    {
        Result.Topology.SubMeshes = MoveTemp(FbxSubMeshes);
        Result.Topology.FaceToSubMesh.Reset();
        Result.Topology.FaceToSubMesh.Reserve(Result.Topology.Faces.Num());
        for (const FNGonSubMeshInfo& Sub : Result.Topology.SubMeshes)
        {
            for (int32 f = Sub.FirstFaceId; f < Sub.FirstFaceId + Sub.FaceCount; ++f)
            {
                if (Result.Topology.Faces.Contains(f))
                {
                    Result.Topology.FaceToSubMesh.Add(f, Sub.SubMeshIndex);
                }
            }
        }
    }

    Error = FText::GetEmpty();
    Out = MoveTemp(Result);
    return true;
}
}

bool NGon::LoadFbx(const FString& Filename, FNGonMeshData& Out, FText& Error)
{
    return LoadFbxInternal(Filename, Out, nullptr, Error);
}

bool NGon::LoadFbxWithSkeleton(
    const FString& Filename,
    FNGonMeshData& OutMesh,
    FNGonFbxSkeletonData& OutSkeleton,
    FText& Error)
{
    return LoadFbxInternal(Filename, OutMesh, &OutSkeleton, Error);
}

#undef LOCTEXT_NAMESPACE
// NGon Unified ABI Sync v5.8.2
