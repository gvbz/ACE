#pragma once
#include "CoreMinimal.h"

/** Local launcher data. Accounts are shared across the user's saved servers. */
struct FACELoginServer
{
	FString Id, Name, Host;
	int32 Port = 9000;
	bool bGDLE = false;
	FString Description, Website, Discord;
};

struct FACELoginAccount
{
	FString Id, Username, Password, Alias;
	/** Alias labels this local entry; authentication always uses Username. */
	FString DisplayName() const;
};

struct FACELoginSettings
{
	// Retained for migration from the original encrypted last-login file.
	FString Host;
	FString Port = TEXT("9000");
	FString Account;
	FString Password;
	TArray<FACELoginServer> Servers;
	TArray<FACELoginAccount> Accounts;
	FString SelectedServerId, SelectedAccountId, DatDirectory;
	bool bProfileFormat = false;
	bool bAutoUpdate = false;
	/** Linux/macOS password saving is opt-in. The local file is readable by this OS user. */
	bool bSavePasswordsLocally = false;
	void MigrateLegacy();
	const FACELoginServer* SelectedServer() const;
	void RemoveServer(const FString& Id);
};

namespace ACELoginProfile
{
	constexpr const TCHAR* DirectoryUrl = TEXT("https://raw.githubusercontent.com/acresources/serverslist/master/Servers.xml");
	bool ValidateServer(const FACELoginServer& Server, FString& Error);
	bool IsWebLink(const FString& Url);
	/** Accept the two public ThwargLauncher schemas; reject unsupported emulators. */
	bool ParseDirectory(const FString& Xml, TArray<FACELoginServer>& OutServers, FString& Error);
	FString DirectoryCachePath();
}
