#include "TiPaintAPI.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FString FTiPaintOpResult::ToJsonString() const
{
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetBoolField(TEXT("ok"), bOk);
    Obj->SetStringField(TEXT("op"), Operation);
    Obj->SetStringField(TEXT("message"), Message);
    if (!PayloadJson.IsEmpty())
    {
        Obj->SetStringField(TEXT("payload"), PayloadJson);
    }
    FString Out;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Obj, Writer);
    return Out;
}

namespace
{
bool ReadColorFromJson(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FColor& OutColor, const FColor& Default)
{
    OutColor = Default;
    if (!Obj.IsValid() || !Obj->HasField(Field)) return false;

    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (Obj->TryGetArrayField(Field, Arr) && Arr && Arr->Num() >= 3)
    {
        auto ToByte = [](double V) -> uint8
        {
            if (V >= 0.0 && V <= 1.0) return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(V * 255.0), 0, 255));
            return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(V), 0, 255));
        };
        const double R = (*Arr)[0]->AsNumber();
        const double G = (*Arr)[1]->AsNumber();
        const double B = (*Arr)[2]->AsNumber();
        const double A = (Arr->Num() >= 4) ? (*Arr)[3]->AsNumber() : 255.0;
        const bool bNormalized = (R <= 1.0 && G <= 1.0 && B <= 1.0 && (Arr->Num() < 4 || A <= 1.0));
        if (bNormalized)
        {
            OutColor = FColor(ToByte(R), ToByte(G), ToByte(B), Arr->Num() >= 4 ? ToByte(A) : 255);
        }
        else
        {
            OutColor = FColor(
                static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(R), 0, 255)),
                static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(G), 0, 255)),
                static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(B), 0, 255)),
                static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(A), 0, 255)));
        }
        return true;
    }

    FString HexStr;
    if (Obj->TryGetStringField(Field, HexStr))
    {
        OutColor = FColor::FromHex(HexStr);
        return true;
    }
    return false;
}

bool ParseBrushMode(const FString& Str, ETiPaintMode& OutMode)
{
    const FString L = Str.ToLower().TrimStartAndEnd();
    if (L == TEXT("paint"))      { OutMode = ETiPaintMode::Paint; return true; }
    if (L == TEXT("erase"))      { OutMode = ETiPaintMode::Erase; return true; }
    if (L == TEXT("smudge"))     { OutMode = ETiPaintMode::Smudge; return true; }
    if (L == TEXT("blur"))       { OutMode = ETiPaintMode::Blur; return true; }
    if (L == TEXT("fill"))       { OutMode = ETiPaintMode::Fill; return true; }
    if (L == TEXT("eyedropper")) { OutMode = ETiPaintMode::Eyedropper; return true; }
    return false;
}

bool ParseBrushTip(const FString& Str, ETiPaintBrushTip& OutTip)
{
    const FString L = Str.ToLower().TrimStartAndEnd();
    if (L == TEXT("round"))        { OutTip = ETiPaintBrushTip::Round; return true; }
    if (L == TEXT("soft") || L == TEXT("airbrush") || L == TEXT("soft_airbrush")) { OutTip = ETiPaintBrushTip::SoftAirbrush; return true; }
    if (L == TEXT("square"))       { OutTip = ETiPaintBrushTip::Square; return true; }
    if (L == TEXT("chalk"))        { OutTip = ETiPaintBrushTip::Chalk; return true; }
    if (L == TEXT("noise"))        { OutTip = ETiPaintBrushTip::Noise; return true; }
    return false;
}

bool ParseSymmetry(const FString& Str, ETiPaintSymmetryMode& OutSym)
{
    const FString L = Str.ToLower().TrimStartAndEnd();
    if (L == TEXT("none"))     { OutSym = ETiPaintSymmetryMode::None; return true; }
    if (L == TEXT("mirror_u") || L == TEXT("u")) { OutSym = ETiPaintSymmetryMode::MirrorU; return true; }
    if (L == TEXT("mirror_v") || L == TEXT("v")) { OutSym = ETiPaintSymmetryMode::MirrorV; return true; }
    if (L == TEXT("radial4") || L == TEXT("radial")) { OutSym = ETiPaintSymmetryMode::Radial4; return true; }
    return false;
}

bool ParseStencilType(const FString& Str, ETiPaintStencilType& OutType)
{
    const FString L = Str.ToLower().TrimStartAndEnd();
    if (L == TEXT("none"))    { OutType = ETiPaintStencilType::None; return true; }
    if (L == TEXT("image"))   { OutType = ETiPaintStencilType::Image; return true; }
    if (L == TEXT("grid"))    { OutType = ETiPaintStencilType::Grid; return true; }
    if (L == TEXT("dots"))    { OutType = ETiPaintStencilType::Dots; return true; }
    if (L == TEXT("stripes")) { OutType = ETiPaintStencilType::Stripes; return true; }
    if (L == TEXT("checker")) { OutType = ETiPaintStencilType::Checker; return true; }
    if (L == TEXT("vignette") || L == TEXT("radial_vignette")) { OutType = ETiPaintStencilType::RadialVignette; return true; }
    if (L == TEXT("noise"))   { OutType = ETiPaintStencilType::Noise; return true; }
    return false;
}

bool ParseProjectionMode(const FString& Str, ETiPaintProjectionMode& OutMode)
{
    const FString L = Str.ToLower().TrimStartAndEnd();
    if (L == TEXT("uv2d") || L == TEXT("uv")) { OutMode = ETiPaintProjectionMode::UV2D; return true; }
    if (L == TEXT("planar_x"))    { OutMode = ETiPaintProjectionMode::PlanarX; return true; }
    if (L == TEXT("planar_y"))    { OutMode = ETiPaintProjectionMode::PlanarY; return true; }
    if (L == TEXT("planar_z"))    { OutMode = ETiPaintProjectionMode::PlanarZ; return true; }
    if (L == TEXT("triplanar"))   { OutMode = ETiPaintProjectionMode::Triplanar; return true; }
    if (L == TEXT("cylindrical")) { OutMode = ETiPaintProjectionMode::Cylindrical; return true; }
    if (L == TEXT("spherical"))   { OutMode = ETiPaintProjectionMode::Spherical; return true; }
    return false;
}

FTiPaintOpResult ExecuteSinglePaintObject(FTiPaintDocument& Doc, const TSharedPtr<FJsonObject>& Obj)
{
    FTiPaintOpResult Res;
    if (!Obj.IsValid())
    {
        Res.Message = TEXT("Invalid JSON command object.");
        return Res;
    }

    FString Op;
    if (!Obj->TryGetStringField(TEXT("op"), Op))
    {
        Obj->TryGetStringField(TEXT("command"), Op);
    }
    Op = Op.ToLower().TrimStartAndEnd();
    Res.Operation = Op;

    auto GetNum = [&](const FString& K, double Def) -> double { double V = Def; Obj->TryGetNumberField(K, V); return V; };
    auto GetInt = [&](const FString& K, int32 Def) -> int32 { double V = static_cast<double>(Def); Obj->TryGetNumberField(K, V); return FMath::RoundToInt(V); };
    auto GetBool = [&](const FString& K, bool Def) -> bool { bool V = Def; Obj->TryGetBoolField(K, V); return V; };
    auto GetStr = [&](const FString& K, const FString& Def) -> FString { FString V = Def; Obj->TryGetStringField(K, V); return V; };

    if (Op == TEXT("inspect"))
    {
        Res.bOk = true;
        Res.Message = TEXT("Exported TiPaint document summary.");
        Res.PayloadJson = FTiPaintAPI::ExportDocumentSummaryJson(Doc);
        return Res;
    }
    if (Op == TEXT("set_brush"))
    {
        FTiPaintBrush B = Doc.Brush();
        FString ModeStr; if (Obj->TryGetStringField(TEXT("mode"), ModeStr)) ParseBrushMode(ModeStr, B.Mode);
        FString TipStr;  if (Obj->TryGetStringField(TEXT("tip"), TipStr)) ParseBrushTip(TipStr, B.Tip);
        FString SymStr;  if (Obj->TryGetStringField(TEXT("symmetry"), SymStr)) ParseSymmetry(SymStr, B.Symmetry);
        ReadColorFromJson(Obj, TEXT("color"), B.Color, B.Color);
        B.Radius = static_cast<float>(FMath::Clamp(GetNum(TEXT("radius"), B.Radius), 1.0, 512.0));
        B.Opacity = static_cast<float>(FMath::Clamp(GetNum(TEXT("opacity"), B.Opacity), 0.0, 1.0));
        B.Flow = static_cast<float>(FMath::Clamp(GetNum(TEXT("flow"), B.Flow), 0.01, 1.0));
        B.Hardness = static_cast<float>(FMath::Clamp(GetNum(TEXT("hardness"), B.Hardness), 0.0, 1.0));
        B.Spacing = static_cast<float>(FMath::Clamp(GetNum(TEXT("spacing"), B.Spacing), 0.05, 2.0));
        B.Scatter = static_cast<float>(FMath::Clamp(GetNum(TEXT("scatter"), B.Scatter), 0.0, 4.0));
        B.JitterSize = static_cast<float>(FMath::Clamp(GetNum(TEXT("jitter_size"), B.JitterSize), 0.0, 1.0));
        B.JitterOpacity = static_cast<float>(FMath::Clamp(GetNum(TEXT("jitter_opacity"), B.JitterOpacity), 0.0, 1.0));
        B.RotationDeg = static_cast<float>(GetNum(TEXT("rotation_deg"), B.RotationDeg));
        Doc.SetBrush(B);
        Res.bOk = true;
        Res.Message = FString::Printf(TEXT("Updated brush (radius %.1f, opacity %.2f, hardness %.2f)."), B.Radius, B.Opacity, B.Hardness);
        return Res;
    }
    if (Op == TEXT("stroke"))
    {
        const TArray<TSharedPtr<FJsonValue>>* PtsArr = nullptr;
        if (!Obj->TryGetArrayField(TEXT("points"), PtsArr) || !PtsArr || PtsArr->IsEmpty())
        {
            Res.Message = TEXT("stroke requires a non-empty 'points' array of [u, v] pairs.");
            return Res;
        }
        TArray<FVector2f> UVs;
        UVs.Reserve(PtsArr->Num());
        for (const TSharedPtr<FJsonValue>& Val : *PtsArr)
        {
            const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
            if (Val.IsValid() && Val->TryGetArray(Pair) && Pair && Pair->Num() >= 2)
            {
                UVs.Add(FVector2f(static_cast<float>((*Pair)[0]->AsNumber()), static_cast<float>((*Pair)[1]->AsNumber())));
            }
        }
        Res.bOk = Doc.DrawStrokePolyline(UVs);
        Res.Message = Res.bOk ? FString::Printf(TEXT("Painted stroke with %d point(s)."), UVs.Num()) : TEXT("Stroke rejected (layer locked or invalid).");
        return Res;
    }
    if (Op == TEXT("draw_line"))
    {
        Res.bOk = Doc.DrawLine(
            static_cast<float>(GetNum(TEXT("u0"), 0.1)), static_cast<float>(GetNum(TEXT("v0"), 0.5)),
            static_cast<float>(GetNum(TEXT("u1"), 0.9)), static_cast<float>(GetNum(TEXT("v1"), 0.5)));
        Res.Message = Res.bOk ? TEXT("Drew line.") : TEXT("Line rejected (layer locked).");
        return Res;
    }
    if (Op == TEXT("draw_rect"))
    {
        Res.bOk = Doc.DrawRect(
            static_cast<float>(GetNum(TEXT("u0"), 0.2)), static_cast<float>(GetNum(TEXT("v0"), 0.2)),
            static_cast<float>(GetNum(TEXT("u1"), 0.8)), static_cast<float>(GetNum(TEXT("v1"), 0.8)),
            GetBool(TEXT("filled"), true));
        Res.Message = Res.bOk ? TEXT("Drew rectangle.") : TEXT("Rectangle rejected (layer locked).");
        return Res;
    }
    if (Op == TEXT("draw_circle"))
    {
        Res.bOk = Doc.DrawCircle(
            static_cast<float>(GetNum(TEXT("u"), 0.5)), static_cast<float>(GetNum(TEXT("v"), 0.5)),
            static_cast<float>(GetNum(TEXT("radius_uv"), 0.2)),
            GetBool(TEXT("filled"), true));
        Res.Message = Res.bOk ? TEXT("Drew circle.") : TEXT("Circle rejected (layer locked).");
        return Res;
    }
    if (Op == TEXT("flood_fill"))
    {
        Res.bOk = Doc.FloodFill(
            static_cast<float>(GetNum(TEXT("u"), 0.5)),
            static_cast<float>(GetNum(TEXT("v"), 0.5)),
            static_cast<float>(GetNum(TEXT("tolerance"), 0.08)));
        Res.Message = Res.bOk ? TEXT("Flood filled region.") : TEXT("Flood fill rejected.");
        return Res;
    }
    if (Op == TEXT("fill_layer"))
    {
        FColor Col = Doc.Brush().Color;
        ReadColorFromJson(Obj, TEXT("color"), Col, Col);
        Doc.FillActiveLayer(Col);
        Res.bOk = true;
        Res.Message = TEXT("Filled active layer.");
        return Res;
    }
    if (Op == TEXT("add_layer"))
    {
        Doc.AddLayer(GetStr(TEXT("name"), FString()));
        Res.bOk = true;
        Res.Message = FString::Printf(TEXT("Added layer '%s'."), *Doc.Layers()[Doc.ActiveLayer()].Name);
        return Res;
    }
    if (Op == TEXT("duplicate_layer")) { Doc.DuplicateActiveLayer(); Res.bOk = true; Res.Message = TEXT("Duplicated active layer."); return Res; }
    if (Op == TEXT("remove_layer"))    { Doc.RemoveActiveLayer(); Res.bOk = true; Res.Message = TEXT("Removed active layer."); return Res; }
    if (Op == TEXT("set_active_layer"))
    {
        const int32 Idx = GetInt(TEXT("index"), 0);
        if (!Doc.Layers().IsValidIndex(Idx)) { Res.Message = TEXT("Layer index out of range."); return Res; }
        Doc.SetActiveLayer(Idx);
        Res.bOk = true; Res.Message = FString::Printf(TEXT("Active layer set to %d."), Idx); return Res;
    }
    if (Op == TEXT("rename_layer"))    { Doc.RenameActiveLayer(GetStr(TEXT("name"), TEXT("Layer"))); Res.bOk = true; Res.Message = TEXT("Renamed layer."); return Res; }
    if (Op == TEXT("set_layer_opacity")) { Doc.SetActiveLayerOpacity(static_cast<float>(GetNum(TEXT("opacity"), 1.0))); Res.bOk = true; Res.Message = TEXT("Updated layer opacity."); return Res; }
    if (Op == TEXT("set_layer_blend_mode"))
    {
        ETiPaintBlendMode Mode = ETiPaintBlendMode::Normal;
        if (!FTiPaintDocument::ParseBlendMode(GetStr(TEXT("blend_mode"), TEXT("normal")), Mode))
        {
            Res.Message = TEXT("Unknown blend_mode (use normal, multiply, screen, overlay, add, darken, lighten).");
            return Res;
        }
        Doc.SetActiveLayerBlendMode(Mode);
        Res.bOk = true; Res.Message = FString::Printf(TEXT("Set layer blend mode to %s."), *FTiPaintDocument::BlendModeToString(Mode)); return Res;
    }
    if (Op == TEXT("move_layer"))      { Res.bOk = Doc.MoveActiveLayer(GetInt(TEXT("delta"), 1)); Res.Message = Res.bOk ? TEXT("Moved layer.") : TEXT("Could not move layer."); return Res; }
    if (Op == TEXT("merge_down"))      { Res.bOk = Doc.MergeActiveLayerDown(); Res.Message = Res.bOk ? TEXT("Merged layer down.") : TEXT("Cannot merge bottom layer."); return Res; }
    if (Op == TEXT("flatten"))         { Doc.FlattenVisibleLayers(); Res.bOk = true; Res.Message = TEXT("Flattened visible layers."); return Res; }
    if (Op == TEXT("toggle_visibility")) { Doc.ToggleActiveVisibility(); Res.bOk = true; Res.Message = TEXT("Toggled layer visibility."); return Res; }
    if (Op == TEXT("toggle_lock"))     { Doc.ToggleActiveLock(); Res.bOk = true; Res.Message = TEXT("Toggled layer lock."); return Res; }
    if (Op == TEXT("toggle_alpha_lock")) { Doc.ToggleActiveAlphaLock(); Res.bOk = true; Res.Message = TEXT("Toggled layer alpha lock."); return Res; }
    if (Op == TEXT("clear_layer"))     { Doc.Clear(); Res.bOk = true; Res.Message = TEXT("Cleared active layer."); return Res; }
    if (Op == TEXT("set_stencil"))
    {
        FTiPaintStencil S = Doc.Stencil();
        S.bEnabled = GetBool(TEXT("enabled"), true);
        FString TypeStr; if (Obj->TryGetStringField(TEXT("type"), TypeStr)) ParseStencilType(TypeStr, S.Type);
        S.Scale = static_cast<float>(FMath::Clamp(GetNum(TEXT("scale"), S.Scale), 0.05, 64.0));
        S.RotationDeg = static_cast<float>(GetNum(TEXT("rotation_deg"), S.RotationDeg));
        S.Opacity = static_cast<float>(FMath::Clamp(GetNum(TEXT("opacity"), S.Opacity), 0.0, 1.0));
        S.bTile = GetBool(TEXT("tile"), S.bTile);
        S.bInvert = GetBool(TEXT("invert"), S.bInvert);
        S.bUseAsColorStamp = GetBool(TEXT("use_color"), S.bUseAsColorStamp);
        const TArray<TSharedPtr<FJsonValue>>* OffArr = nullptr;
        if (Obj->TryGetArrayField(TEXT("offset"), OffArr) && OffArr && OffArr->Num() >= 2)
        {
            S.Offset = FVector2f(static_cast<float>((*OffArr)[0]->AsNumber()), static_cast<float>((*OffArr)[1]->AsNumber()));
        }
        Doc.SetStencil(S);
        FString ImgPath;
        if (Obj->TryGetStringField(TEXT("image_path"), ImgPath) && !ImgPath.IsEmpty())
        {
            FText Err;
            if (!Doc.LoadStencilImage(ImgPath, Err))
            {
                Res.Message = Err.ToString();
                return Res;
            }
        }
        Res.bOk = true;
        Res.Message = S.bEnabled ? TEXT("Stencil enabled and updated.") : TEXT("Stencil disabled.");
        return Res;
    }
    if (Op == TEXT("project_stencil") || Op == TEXT("project"))
    {
        FTiPaintProjectionSettings P = Doc.Projection();
        FString ModeStr; if (Obj->TryGetStringField(TEXT("mode"), ModeStr)) ParseProjectionMode(ModeStr, P.Mode);
        P.Scale = static_cast<float>(FMath::Clamp(GetNum(TEXT("scale"), P.Scale), 0.05, 64.0));
        P.BlendSharpness = static_cast<float>(FMath::Clamp(GetNum(TEXT("blend_sharpness"), P.BlendSharpness), 1.0, 16.0));
        P.Opacity = static_cast<float>(FMath::Clamp(GetNum(TEXT("opacity"), P.Opacity), 0.0, 1.0));
        P.bUseStencil = GetBool(TEXT("use_stencil"), P.bUseStencil);
        P.SeamDilationPixels = FMath::Clamp(GetInt(TEXT("dilate_seams"), P.SeamDilationPixels), 0, 8);
        ReadColorFromJson(Obj, TEXT("color"), P.TintColor, Doc.Brush().Color);
        Doc.SetProjection(P);
        Res.bOk = Doc.ProjectOntoMesh(P, Res.Message);
        return Res;
    }
    if (Op == TEXT("stamp_3d"))
    {
        FVector3f Pos = FVector3f::ZeroVector;
        FVector3f Norm = FVector3f::ZeroVector;
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Obj->TryGetArrayField(TEXT("position"), Arr) && Arr && Arr->Num() >= 3)
        {
            Pos = FVector3f(static_cast<float>((*Arr)[0]->AsNumber()), static_cast<float>((*Arr)[1]->AsNumber()), static_cast<float>((*Arr)[2]->AsNumber()));
        }
        if (Obj->TryGetArrayField(TEXT("normal"), Arr) && Arr && Arr->Num() >= 3)
        {
            Norm = FVector3f(static_cast<float>((*Arr)[0]->AsNumber()), static_cast<float>((*Arr)[1]->AsNumber()), static_cast<float>((*Arr)[2]->AsNumber()));
        }
        const float Rad3D = static_cast<float>(GetNum(TEXT("radius_3d"), 20.0));
        Res.bOk = Doc.StampWorld3D(Pos, Norm, Rad3D, Res.Message);
        return Res;
    }
    if (Op == TEXT("dilate_seams"))
    {
        Res.bOk = Doc.DilateSeams(GetInt(TEXT("iterations"), 2));
        Res.Message = Res.bOk ? TEXT("Dilated UV seams.") : TEXT("Seam dilation rejected (layer locked).");
        return Res;
    }
    if (Op == TEXT("resize"))
    {
        const int32 W = GetInt(TEXT("width"), Doc.Width());
        const int32 H = GetInt(TEXT("height"), Doc.Height());
        Res.bOk = Doc.Resize(W, H);
        Res.Message = FString::Printf(TEXT("Resized canvas to %dx%d."), Doc.Width(), Doc.Height());
        return Res;
    }
    if (Op == TEXT("export_png"))
    {
        FText Err;
        const FString Path = GetStr(TEXT("path"), FString());
        Res.bOk = !Path.IsEmpty() && Doc.ExportPNG(Path, Err);
        Res.Message = Res.bOk ? FString::Printf(TEXT("Exported PNG to %s."), *Path) : Err.ToString();
        return Res;
    }
    if (Op == TEXT("import_layer_image"))
    {
        FText Err;
        const FString Path = GetStr(TEXT("path"), FString());
        Res.bOk = !Path.IsEmpty() && Doc.ImportImageToNewLayer(Path, Err);
        Res.Message = Res.bOk ? FString::Printf(TEXT("Imported image layer from %s."), *Path) : Err.ToString();
        return Res;
    }
    if (Op == TEXT("undo")) { Doc.Undo(); Res.bOk = true; Res.Message = TEXT("Undo executed."); return Res; }
    if (Op == TEXT("redo")) { Doc.Redo(); Res.bOk = true; Res.Message = TEXT("Redo executed."); return Res; }

    Res.Message = FString::Printf(TEXT("Unknown TiPaint operation '%s'."), *Op);
    return Res;
}
} // namespace

FString FTiPaintAPI::GetApiSchemaJson()
{
    return TEXT(
        "{\n"
        "  \"api\": \"TiPaintAPI\",\n"
        "  \"version\": \"2.0\",\n"
        "  \"format\": \"Single JSON object {\\\"op\\\": ...} or batch array [{\\\"op\\\": ...}, ...]\",\n"
        "  \"operations\": [\n"
        "    {\"op\": \"set_brush\", \"params\": {\"mode\": \"paint|erase|smudge|blur|fill|eyedropper\", \"color\": [77,128,255,255], \"radius\": 24, \"opacity\": 1.0, \"flow\": 1.0, \"hardness\": 0.7, \"spacing\": 0.2, \"tip\": \"round|soft|square|chalk|noise\", \"symmetry\": \"none|mirror_u|mirror_v|radial4\", \"scatter\": 0.0}},\n"
        "    {\"op\": \"stroke\", \"params\": {\"points\": [[0.2,0.5],[0.8,0.5]]}},\n"
        "    {\"op\": \"draw_line\", \"params\": {\"u0\": 0.1, \"v0\": 0.5, \"u1\": 0.9, \"v1\": 0.5}},\n"
        "    {\"op\": \"draw_rect\", \"params\": {\"u0\": 0.2, \"v0\": 0.2, \"u1\": 0.8, \"v1\": 0.8, \"filled\": true}},\n"
        "    {\"op\": \"draw_circle\", \"params\": {\"u\": 0.5, \"v\": 0.5, \"radius_uv\": 0.2, \"filled\": true}},\n"
        "    {\"op\": \"flood_fill\", \"params\": {\"u\": 0.5, \"v\": 0.5, \"tolerance\": 0.08}},\n"
        "    {\"op\": \"fill_layer\", \"params\": {\"color\": [255,255,255,255]}},\n"
        "    {\"op\": \"add_layer\", \"params\": {\"name\": \"Detail\"}},\n"
        "    {\"op\": \"duplicate_layer\"}, {\"op\": \"remove_layer\"}, {\"op\": \"merge_down\"}, {\"op\": \"flatten\"},\n"
        "    {\"op\": \"set_active_layer\", \"params\": {\"index\": 0}},\n"
        "    {\"op\": \"set_layer_opacity\", \"params\": {\"opacity\": 0.8}},\n"
        "    {\"op\": \"set_layer_blend_mode\", \"params\": {\"blend_mode\": \"normal|multiply|screen|overlay|add|darken|lighten\"}},\n"
        "    {\"op\": \"toggle_visibility\"}, {\"op\": \"toggle_lock\"}, {\"op\": \"toggle_alpha_lock\"}, {\"op\": \"clear_layer\"},\n"
        "    {\"op\": \"set_stencil\", \"params\": {\"enabled\": true, \"type\": \"checker|grid|dots|stripes|vignette|noise|image\", \"scale\": 4.0, \"rotation_deg\": 0, \"invert\": false, \"tile\": true, \"use_color\": false, \"image_path\": \"\"}},\n"
        "    {\"op\": \"project_stencil\", \"params\": {\"mode\": \"triplanar|planar_x|planar_y|planar_z|cylindrical|spherical|uv2d\", \"scale\": 1.0, \"blend_sharpness\": 4.0, \"opacity\": 1.0, \"color\": [255,255,255,255], \"dilate_seams\": 2}},\n"
        "    {\"op\": \"stamp_3d\", \"params\": {\"position\": [0,0,50], \"normal\": [0,0,1], \"radius_3d\": 25.0}},\n"
        "    {\"op\": \"dilate_seams\", \"params\": {\"iterations\": 2}},\n"
        "    {\"op\": \"resize\", \"params\": {\"width\": 1024, \"height\": 1024}},\n"
        "    {\"op\": \"export_png\", \"params\": {\"path\": \"C:/out.png\"}},\n"
        "    {\"op\": \"import_layer_image\", \"params\": {\"path\": \"C:/in.png\"}},\n"
        "    {\"op\": \"undo\"}, {\"op\": \"redo\"}, {\"op\": \"inspect\"}\n"
        "  ]\n"
        "}");
}

FString FTiPaintAPI::ExportDocumentSummaryJson(const FTiPaintDocument& Document)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("width"), Document.Width());
    Root->SetNumberField(TEXT("height"), Document.Height());
    Root->SetNumberField(TEXT("revision"), static_cast<double>(Document.Revision()));
    Root->SetNumberField(TEXT("active_layer"), Document.ActiveLayer());
    Root->SetNumberField(TEXT("undo_count"), Document.UndoCount());
    Root->SetNumberField(TEXT("redo_count"), Document.RedoCount());
    Root->SetNumberField(TEXT("mesh_triangles"), Document.MeshTriangles().Num());

    TArray<TSharedPtr<FJsonValue>> LayersJson;
    for (int32 i = 0; i < Document.Layers().Num(); ++i)
    {
        const FTiPaintLayer& L = Document.Layers()[i];
        TSharedRef<FJsonObject> LO = MakeShared<FJsonObject>();
        LO->SetNumberField(TEXT("index"), i);
        LO->SetStringField(TEXT("name"), L.Name);
        LO->SetBoolField(TEXT("visible"), L.bVisible);
        LO->SetBoolField(TEXT("locked"), L.bLocked);
        LO->SetBoolField(TEXT("alpha_lock"), L.bAlphaLock);
        LO->SetNumberField(TEXT("opacity"), L.Opacity);
        LO->SetStringField(TEXT("blend_mode"), FTiPaintDocument::BlendModeToString(L.BlendMode));
        LayersJson.Add(MakeShared<FJsonValueObject>(LO));
    }
    Root->SetArrayField(TEXT("layers"), LayersJson);

    const FTiPaintBrush& B = Document.Brush();
    TSharedRef<FJsonObject> BObj = MakeShared<FJsonObject>();
    BObj->SetNumberField(TEXT("radius"), B.Radius);
    BObj->SetNumberField(TEXT("opacity"), B.Opacity);
    BObj->SetNumberField(TEXT("flow"), B.Flow);
    BObj->SetNumberField(TEXT("hardness"), B.Hardness);
    BObj->SetNumberField(TEXT("spacing"), B.Spacing);
    Root->SetObjectField(TEXT("brush"), BObj);

    const FTiPaintStencil& S = Document.Stencil();
    TSharedRef<FJsonObject> SObj = MakeShared<FJsonObject>();
    SObj->SetBoolField(TEXT("enabled"), S.bEnabled);
    SObj->SetNumberField(TEXT("scale"), S.Scale);
    SObj->SetBoolField(TEXT("invert"), S.bInvert);
    Root->SetObjectField(TEXT("stencil"), SObj);

    FString Out;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Root, Writer);
    return Out;
}

FTiPaintOpResult FTiPaintAPI::ExecuteJsonCommand(FTiPaintDocument& Document, const FString& JsonText)
{
    FTiPaintOpResult Res;
    Res.Operation = TEXT("json_command");
    const FString Trimmed = JsonText.TrimStartAndEnd();
    if (Trimmed.IsEmpty())
    {
        Res.Message = TEXT("JSON input is empty.");
        return Res;
    }

    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
    if (Trimmed.StartsWith(TEXT("[")))
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        if (!FJsonSerializer::Deserialize(Reader, Arr))
        {
            Res.Message = TEXT("Failed to parse JSON command array.");
            return Res;
        }
        int32 Executed = 0;
        FTiPaintOpResult Last;
        for (int32 i = 0; i < Arr.Num(); ++i)
        {
            const TSharedPtr<FJsonObject>* SubObj = nullptr;
            if (!Arr[i].IsValid() || !Arr[i]->TryGetObject(SubObj) || !SubObj)
            {
                Res.Message = FString::Printf(TEXT("Batch step %d is not a valid JSON object."), i);
                return Res;
            }
            Last = ExecuteSinglePaintObject(Document, *SubObj);
            if (!Last.bOk)
            {
                Res.Operation = Last.Operation;
                Res.Message = FString::Printf(TEXT("Batch failed at step %d (%s): %s"), i, *Last.Operation, *Last.Message);
                return Res;
            }
            ++Executed;
        }
        Res.bOk = true;
        Res.Operation = TEXT("json_batch");
        Res.Message = FString::Printf(TEXT("Executed %d paint command(s). Last: %s"), Executed, *Last.Message);
        Res.PayloadJson = Last.PayloadJson;
        return Res;
    }

    TSharedPtr<FJsonObject> Obj;
    if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
    {
        Res.Message = TEXT("Failed to parse JSON command object.");
        return Res;
    }
    return ExecuteSinglePaintObject(Document, Obj);
}
