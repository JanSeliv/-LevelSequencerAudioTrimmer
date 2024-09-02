// Copyright (c) Yevhenii Selivanov

#include "LevelSequencerAudioTrimmerEdModule.h"
//---
#include "LSATUtilsLibrary.h"
//---
#include "Editor.h"
#include "LevelSequence.h"
#include "ToolMenus.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FLevelSequencerAudioTrimmerEdModule, LevelSequencerAudioTrimmer)

// Current path to the plugin
FString FLevelSequencerAudioTrimmerEdModule::PluginPath = TEXT("");

// Current path to the FFMPEG library 
FString FLevelSequencerAudioTrimmerEdModule::FfmpegPath = TEXT("");

// Called right after the module DLL has been loaded and the module object has been created
void FLevelSequencerAudioTrimmerEdModule::StartupModule()
{
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FLevelSequencerAudioTrimmerEdModule::RegisterMenus));

	InitPluginPath();
	InitFfmpegPath();
}

// Called before the module is unloaded, right before the module object is destroyed
void FLevelSequencerAudioTrimmerEdModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

// Registers the custom context menu item for Level Sequence assets
void FLevelSequencerAudioTrimmerEdModule::RegisterMenus()
{
	// Extend the context menu for Level Sequence assets
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu.LevelSequence");
	FToolMenuSection& Section = Menu->FindOrAddSection("AssetContextLevelSequence");
	Section.AddMenuEntry(
		"LevelSequencerAudioTrimmer",
		NSLOCTEXT("LevelSequencerAudioTrimmer", "LevelSequencerAudioTrimmer_Label", "Level Sequencer Audio Trimmer"),
		NSLOCTEXT("LevelSequencerAudioTrimmer", "LevelSequencerAudioTrimmer_Tooltip", "Trims audio tracks in the Level Sequence"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FLevelSequencerAudioTrimmerEdModule::OnLevelSequencerAudioTrimmerClicked))
	);
}

// Is called when Audio Trimmer button in clicked in the context menu of the Level Sequence asset
void FLevelSequencerAudioTrimmerEdModule::OnLevelSequencerAudioTrimmerClicked()
{
	// Get the selected assets from the content browser
	TArray<FAssetData> SelectedAssets;
	GEditor->GetContentBrowserSelections(SelectedAssets);

	TArray<ULevelSequence*> LevelSequences;
	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (ULevelSequence* LevelSequence = Cast<ULevelSequence>(AssetData.GetAsset()))
		{
			LevelSequences.Add(LevelSequence);
		}
	}

	if (!LevelSequences.IsEmpty())
	{
		ULSATUtilsLibrary::RunLevelSequenceAudioTrimmer(LevelSequences);
	}
}

/*********************************************************************************************
 * Plugin name/path
 ********************************************************************************************* */

// Current path to this plugin
void FLevelSequencerAudioTrimmerEdModule::InitPluginPath()
{
	const IPlugin* PluginPtr = IPluginManager::Get().FindPlugin(PluginName).Get();
	checkf(PluginPtr, TEXT("ERROR: [%i] %hs:\n'PluginPtr' is null!"), __LINE__, __FUNCTION__);
	const FString RelativePluginPath = PluginPtr->GetBaseDir();
	PluginPath = FPaths::ConvertRelativePathToFull(RelativePluginPath);
}

/*********************************************************************************************
 * FFMPEG
 ********************************************************************************************* */

// Sets FFMPEG path depending on the platform
void FLevelSequencerAudioTrimmerEdModule::InitFfmpegPath()
{
	checkf(!PluginPath.IsEmpty(), TEXT("ERROR: [%i] %hs:\n'PluginPath' is empty!"), __LINE__, __FUNCTION__);

	FString RelativePath;
#if PLATFORM_WINDOWS
	static const FString WinPath = TEXT("ThirdParty/ffmpeg/Windows/ffmpeg.exe");
	RelativePath = FPaths::Combine(PluginPath, WinPath);
#elif PLATFORM_MAC
	static const FString MacPath = TEXT("ThirdParty/ffmpeg/Mac/ffmpeg");
	RelativePath = FPaths::Combine(PluginPath, MacPath);
#elif PLATFORM_LINUX
	static const FString LinuxPath = TEXT("ThirdParty/ffmpeg/Linux/ffmpeg");
	RelativePath = FPaths::Combine(PluginPath, LinuxPath);
#endif

	// Convert the relative path to an absolute path
	FfmpegPath = FPaths::ConvertRelativePathToFull(RelativePath);
}
