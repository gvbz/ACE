#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UWorld;
struct FACEClientPlugin;

/**
 * Plugin service "dev" (permission dev_tools): reads and sets the personal. console variables
 * and runs the personal. console commands, for the Development Dashboard. Nothing outside
 * personal. can be read, set or run.
 *
 * Operations (request field "operation"):
 * - list: every personal. variable and command, in "entries".
 * - get: "names" (array) -> "values" (object of name to value).
 * - set: "values" (object of name to value) -> "values" with the new values. All or nothing.
 * - run: "text", one personal. command with its arguments -> "command" and "output".
 */
namespace PersonalDevService
{
	constexpr const TCHAR* Permission = TEXT("dev_tools");
	constexpr const TCHAR* Prefix = TEXT("personal.");

	/** Same contract as the other plugin services: true with Reply filled, or false with Error set. */
	bool Handle(const FACEClientPlugin& Plugin, const TSharedPtr<FJsonObject>& Request, const TSharedPtr<FJsonObject>& Reply, FString& Error, UWorld* World);
}
