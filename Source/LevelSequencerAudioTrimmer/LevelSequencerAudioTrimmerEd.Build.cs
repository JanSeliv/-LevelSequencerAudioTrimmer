﻿// Copyright (c) Yevhenii Selivanov

using UnrealBuildTool;

public class LevelSequencerAudioTrimmerEd : ModuleRules
{
	public LevelSequencerAudioTrimmerEd(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Latest;
		bEnableNonInlinedGenCppWarnings = true;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"CoreUObject", "Engine", "Slate", "SlateCore" // Core
				, "MovieSceneTracks" // UMovieSceneAudioSection
				, "MovieScene"
				, "LevelSequence"
				, "UnrealEd" // FReimportManager
			}
		);
	}
}
