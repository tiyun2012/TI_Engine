#pragma once

#include "CoreMinimal.h"

/** Active brush tool mode. */
enum class ETiPaintMode : uint8
{
    Paint,
    Erase,
    Smudge,
    Blur,
    Fill,
    Eyedropper
};

/** Per-layer Porter-Duff / Photoshop-style blend mode. */
enum class ETiPaintBlendMode : uint8
{
    Normal,
    Multiply,
    Screen,
    Overlay,
    Add,
    Darken,
    Lighten
};

/** Procedural brush tip shape. */
enum class ETiPaintBrushTip : uint8
{
    Round,
    SoftAirbrush,
    Square,
    Chalk,
    Noise
};

/** Stroke symmetry axis mode in UV space. */
enum class ETiPaintSymmetryMode : uint8
{
    None,
    MirrorU,
    MirrorV,
    Radial4
};

/** Procedural or image-backed stencil pattern. */
enum class ETiPaintStencilType : uint8
{
    None,
    Image,
    Grid,
    Dots,
    Stripes,
    Checker,
    RadialVignette,
    Noise
};

/** Projection mapping mode for 3D-to-UV painting and stencil baking. */
enum class ETiPaintProjectionMode : uint8
{
    UV2D,
    PlanarX,
    PlanarY,
    PlanarZ,
    Triplanar,
    Cylindrical,
    Spherical
};

/**
 * Authored mesh triangle keeping the painter aligned with the active N-Gon mesh.
 * UVs use 0..1 convention. Optional 3D Pos/Normal enable seamless 3D & Triplanar projection.
 */
struct FTiPaintSubMeshInfo
{
    FString Name;
    int32 TriangleCount = 0;
};

struct FTiPaintUVTriangle
{
    FVector2f UV[3] = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
    FVector3f Pos[3] = { FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector };
    FVector3f Normal[3] = { FVector3f::UpVector, FVector3f::UpVector, FVector3f::UpVector };
    int32 SubMeshIndex = 0;
    int32 FaceId = INDEX_NONE;
    bool bHas3D = false;
};

/** Complete brush configuration (safe value struct). */
struct FTiPaintBrush
{
    FColor Color = FColor(77, 128, 255, 255);
    float Radius = 24.0f;
    float Opacity = 1.0f;
    float Flow = 1.0f;
    float Hardness = 0.7f;
    float Spacing = 0.20f;
    float Scatter = 0.0f;
    float JitterSize = 0.0f;
    float JitterOpacity = 0.0f;
    float RotationDeg = 0.0f;
    ETiPaintMode Mode = ETiPaintMode::Paint;
    ETiPaintBrushTip Tip = ETiPaintBrushTip::Round;
    ETiPaintSymmetryMode Symmetry = ETiPaintSymmetryMode::None;
};

/** Stencil mask / color-stamp configuration. */
struct FTiPaintStencil
{
    bool bEnabled = false;
    ETiPaintStencilType Type = ETiPaintStencilType::Checker;
    FVector2f Offset = FVector2f::ZeroVector;
    float Scale = 4.0f;
    float RotationDeg = 0.0f;
    float Opacity = 1.0f;
    bool bTile = true;
    bool bInvert = false;
    bool bUseAsColorStamp = false;

    // Optional loaded RGBA image stencil (populated via LoadStencilImage)
    FString ImagePath;
    int32 ImageWidth = 0;
    int32 ImageHeight = 0;
    TArray<FColor> ImagePixels;
};

/** Settings for 3D projection onto the active mesh's UV triangles. */
struct FTiPaintProjectionSettings
{
    ETiPaintProjectionMode Mode = ETiPaintProjectionMode::Triplanar;
    float Scale = 1.0f;
    float BlendSharpness = 4.0f;
    float Opacity = 1.0f;
    FVector3f Offset = FVector3f::ZeroVector;
    FColor TintColor = FColor::White;
    bool bUseStencil = true;
    int32 SeamDilationPixels = 2;
};

/** Single raster paint layer with blend mode, opacity, and alpha-lock protection. */
struct FTiPaintLayer
{
    FString Name;
    bool bVisible = true;
    bool bLocked = false;
    bool bAlphaLock = false;
    float Opacity = 1.0f;
    ETiPaintBlendMode BlendMode = ETiPaintBlendMode::Normal;
    TArray<FColor> Pixels;
};

/** Structured result returned by TiPaint AI / JSON API commands. */
struct FTiPaintOpResult
{
    bool bOk = false;
    FString Operation;
    FString Message;
    FString PayloadJson;

    FString ToJsonString() const;
};
