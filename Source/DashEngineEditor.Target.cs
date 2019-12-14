// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;
using System.Collections.Generic;

public class DashEngineEditorTarget : TargetRules
{
	public DashEngineEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
        bIWYU = true;
        ExtraModuleNames.AddRange( new string[] { "DashEngine" } );
	}
}
