#include "Mods/ACEPluginPresentation.h"
#include "Mods/ACEPluginIcon.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"

namespace
{
    FString PresentationValue(const FACEClientPlugin& P,const TCHAR* Key)
    {
        FString Text;
        if(P.Presentation&&P.Presentation->TryGetStringField(Key,Text))return Text;
        const TSharedPtr<FJsonObject>* Defaults=nullptr;
        if(P.Manifest&&P.Manifest->TryGetObjectField(TEXT("presentation"),Defaults))(*Defaults)->TryGetStringField(Key,Text);
        return Text;
    }
    bool Flag(const FACEClientPlugin& P,const TCHAR* Key)
    {
        bool Result=true;
        if(P.Presentation&&P.Presentation->TryGetBoolField(Key,Result))return Result;
        const TSharedPtr<FJsonObject>* Defaults=nullptr;
        if(P.Manifest&&P.Manifest->TryGetObjectField(TEXT("presentation"),Defaults))(*Defaults)->TryGetBoolField(Key,Result);
        return Result;
    }
}
FString FACEClientPlugin::DisplayTitle() const
{const FString Title=PresentationValue(*this,TEXT("title"));return Title.IsEmpty()?Name:Title;}
FString FACEClientPlugin::DisplayIcon() const {return PresentationValue(*this,TEXT("icon"));}
FString FACEClientPlugin::DisplayControls() const {const auto V=PresentationValue(*this,TEXT("controls"));return V.IsEmpty()?TEXT("start_stop"):V;}
FString FACEClientPlugin::DisplayStartLabel() const {const auto V=PresentationValue(*this,TEXT("start_label"));return V.IsEmpty()?TEXT("Start"):V;}
FString FACEClientPlugin::DisplayStopLabel() const {const auto V=PresentationValue(*this,TEXT("stop_label"));return V.IsEmpty()?TEXT("Stop"):V;}
bool FACEClientPlugin::ShowSettings() const {return Flag(*this,TEXT("settings"));}
bool FACEClientPlugin::ShowStatus() const {return Flag(*this,TEXT("status"));}
bool FACEClientPlugin::ScrollPanel() const {return Flag(*this,TEXT("scroll"));}

bool ACEPluginPresentation::ValidAsset(const FString& Asset)
{
    return !Asset.IsEmpty()&&Asset.Len()<=128&&Asset.EndsWith(TEXT(".png"))
        &&Asset==FPaths::GetCleanFilename(Asset)&&!Asset.Contains(TEXT(":"))
        &&!Asset.Contains(TEXT("/"))&&!Asset.Contains(TEXT("\\"))&&!Asset.Contains(TEXT(".."));
}
bool ACEPluginPresentation::Valid(const TSharedPtr<FJsonObject>& O,FString& Error)
{
    if(!O){Error=TEXT("presentation must be an object");return false;}
    for(const auto& Pair:O->Values)
    {
        if(Pair.Key==TEXT("settings")||Pair.Key==TEXT("status")||Pair.Key==TEXT("scroll"))
        {
            if(Pair.Value->Type==EJson::Boolean)continue;
            Error=TEXT("presentation settings/status/scroll must be booleans");return false;
        }
        if(Pair.Key==TEXT("controls"))
        {
            FString Mode;
            if(Pair.Value->TryGetString(Mode)&&(Mode==TEXT("start_stop")||Mode==TEXT("toggle")||Mode==TEXT("none")))continue;
            Error=TEXT("presentation.controls must be start_stop, toggle or none");return false;
        }
        if(Pair.Key==TEXT("start_label")||Pair.Key==TEXT("stop_label"))
        {
            FString Label;
            if(!Pair.Value->TryGetString(Label)||Label.Len()>40){Error=TEXT("Start/Stop labels must be strings of at most 40 characters");return false;}
            for(TCHAR Ch:Label)if(FChar::IsControl(Ch)){Error=TEXT("Start/Stop labels must be a single line");return false;}
            continue;
        }
        if(Pair.Key==TEXT("window"))
        {
            const TSharedPtr<FJsonObject>* Window=nullptr;
            if(!Pair.Value->TryGetObject(Window)){Error=TEXT("presentation.window must be an object");return false;}
            for(const auto& Field:(*Window)->Values)
            {
                double Opacity=0;bool Pinned=false;
                if(Field.Key==TEXT("opacity")&&Field.Value->Type==EJson::Number&&Field.Value->TryGetNumber(Opacity)&&FMath::IsFinite(Opacity)&&Opacity>=0&&Opacity<=1)continue;
                if(Field.Key==TEXT("pinned")&&Field.Value->Type==EJson::Boolean&&Field.Value->TryGetBool(Pinned))continue;
                Error=TEXT("presentation.window accepts opacity (0..1) and pinned (boolean)");return false;
            }
            continue;
        }
        FString Text;
        if((Pair.Key!=TEXT("title")&&Pair.Key!=TEXT("icon"))||!Pair.Value->TryGetString(Text))
        {Error=TEXT("Unknown presentation field or invalid field type");return false;}
        if(Pair.Key==TEXT("title"))
        {
            if(Text.Len()>80){Error=TEXT("Plugin title is limited to 80 characters");return false;}
            for(TCHAR Ch:Text)if(FChar::IsControl(Ch)){Error=TEXT("Plugin title must be a single line");return false;}
        }
        else if(!Text.IsEmpty()&&(Text.Len()>96||!ValidAsset(Text))){Error=TEXT("Plugin icon must be a PNG filename in the plugin's images folder");return false;}
    }
    return true;
}
UTexture2D* FACEPluginImageCache::Load(const FString& Directory,const FString& Asset)
{
    if(const auto* Cached=Images.Find(Asset))return Cached->Get();
    if(Images.Num()>=32||!ACEPluginPresentation::ValidAsset(Asset))return nullptr;
    // Remember missing/bad assets too; Lua refreshes must not repeatedly hit disk.
    Images.Add(Asset,TStrongObjectPtr<UTexture2D>());
    const FString Path=Directory/TEXT("images")/Asset;const int64 Size=IFileManager::Get().FileSize(*Path);
    if(Size<=0||Size>2*1024*1024)return nullptr;TArray<uint8> Bytes;if(!FFileHelper::LoadFileToArray(Bytes,*Path))return nullptr;
    auto& Module=FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));auto Decoder=Module.CreateImageWrapper(EImageFormat::PNG);
    if(!Decoder||!Decoder->SetCompressed(Bytes.GetData(),Bytes.Num())||Decoder->GetWidth()>1024||Decoder->GetHeight()>1024||Decoder->GetWidth()<1||Decoder->GetHeight()<1)return nullptr;
    const int64 Decoded=int64(Decoder->GetWidth())*Decoder->GetHeight()*4;if(ImageBytes+Decoded>16*1024*1024)return nullptr;
    auto* Texture=FImageUtils::ImportBufferAsTexture2D(Bytes);if(!Texture)return nullptr;
    ImageBytes+=Decoded;Images.Add(Asset,TStrongObjectPtr<UTexture2D>(Texture));return Texture;
}
void SACEPluginBadge::Construct(const FArguments& A){Plugin=A._Plugin;Refresh();}
void SACEPluginBadge::Tick(const FGeometry& G,double T,float D){SCompoundWidget::Tick(G,T,D);Refresh();}
void SACEPluginBadge::Refresh()
{
    if(!Plugin)return;
    const FString Next=Plugin->DisplayIcon();if(Initialized&&Next==Asset)return;Asset=Next;Initialized=true;
    if(!Plugin->Images)Plugin->Images=MakeShared<FACEPluginImageCache>();
    if(auto* Texture=Plugin->Images->Load(Plugin->Directory,Asset))
    {
        Brush.SetResourceObject(Texture);Brush.ImageSize=FVector2D(Texture->GetSizeX(),Texture->GetSizeY());
        ChildSlot[SNew(SBox).WidthOverride(26).HeightOverride(26)
            [SNew(SScaleBox).Stretch(EStretch::ScaleToFit)[SNew(SImage).Image(&Brush)]]];
    }
    else ChildSlot[SNew(SBox).WidthOverride(26).HeightOverride(26)[SNew(SACEPluginIcon).Id(Plugin->Id)]];
}
