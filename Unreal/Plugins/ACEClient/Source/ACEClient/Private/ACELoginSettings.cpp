#include "ACELoginSettings.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include <dpapi.h>
#elif PLATFORM_ANDROID
#include "Android/AndroidApplication.h"
#include "Android/AndroidJNI.h"
#elif PLATFORM_LINUX || PLATFORM_MAC
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

namespace ACELoginSettings
{
#if PLATFORM_ANDROID
	static bool AndroidCrypt(const uint8* Data, int32 Size, bool Encrypt, TArray<uint8>& Output)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		if (!Env || Size < 0 || Size > 1048576 || !FJavaWrapper::GameActivityThis) return false;
		static jmethodID Method = FJavaWrapper::FindMethod(Env, FJavaWrapper::GameActivityClassID,
			"AndroidThunkJava_ACELoginCrypt", "([BZ)[B", false);
		if (!Method) return false;
		jbyteArray Input = Env->NewByteArray(Size);
		if (!Input) return false;
		Env->SetByteArrayRegion(Input, 0, Size, reinterpret_cast<const jbyte*>(Data));
		auto Result = static_cast<jbyteArray>(Env->CallObjectMethod(FJavaWrapper::GameActivityThis, Method, Input, jboolean(Encrypt)));
		Env->DeleteLocalRef(Input);
		if (Env->ExceptionCheck()) { Env->ExceptionClear(); return false; }
		if (!Result) return false;
		const int32 Length = Env->GetArrayLength(Result);
		const bool Valid = Length > 0 && Length <= 1048576;
		if (Valid) { Output.SetNumUninitialized(Length); Env->GetByteArrayRegion(Result, 0, Length, reinterpret_cast<jbyte*>(Output.GetData())); }
		Env->DeleteLocalRef(Result);
		return Valid;
	}
#endif
	FString GetPath()
	{
		return FPaths::ProjectSavedDir() / TEXT("Login/LastLogin.dat");
	}

	bool Save(const FACELoginSettings& Settings, const FString& Path)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("host"), Settings.Host);
		Json->SetStringField(TEXT("port"), Settings.Port);
		Json->SetStringField(TEXT("account"), Settings.Account);
		const bool SavePasswords = !(PLATFORM_LINUX || PLATFORM_MAC) || Settings.bSavePasswordsLocally;
		Json->SetBoolField(TEXT("savePasswordsLocally"), Settings.bSavePasswordsLocally);
		Json->SetStringField(TEXT("password"), SavePasswords ? Settings.Password : FString());
		if (Settings.bProfileFormat)
		{
			Json->SetNumberField(TEXT("version"), 3);
			Json->SetStringField(TEXT("selectedServer"), Settings.SelectedServerId);
			Json->SetStringField(TEXT("selectedAccount"), Settings.SelectedAccountId);
			Json->SetStringField(TEXT("datDirectory"), Settings.DatDirectory);
			Json->SetBoolField(TEXT("autoUpdate"), Settings.bAutoUpdate);
			TArray<TSharedPtr<FJsonValue>> Servers, Accounts;
			for (const auto& S : Settings.Servers)
			{
				auto J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("id"), S.Id); J->SetStringField(TEXT("name"), S.Name);
				J->SetStringField(TEXT("host"), S.Host); J->SetNumberField(TEXT("port"), S.Port);
				J->SetBoolField(TEXT("gdle"), S.bGDLE); J->SetStringField(TEXT("description"), S.Description);
				J->SetStringField(TEXT("website"), S.Website); J->SetStringField(TEXT("discord"), S.Discord);
				Servers.Add(MakeShared<FJsonValueObject>(J));
			}
			for (const auto& A : Settings.Accounts)
			{
				auto J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("id"), A.Id);
				J->SetStringField(TEXT("alias"), A.Alias);
				J->SetStringField(TEXT("username"), A.Username); J->SetStringField(TEXT("password"), SavePasswords ? A.Password : FString());
				Accounts.Add(MakeShared<FJsonValueObject>(J));
			}
			Json->SetArrayField(TEXT("servers"), Servers); Json->SetArrayField(TEXT("accounts"), Accounts);
		}
		FString Text;
		if (!FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text))) return false;
		const FTCHARToUTF8 Utf8(*Text);
		if (Utf8.Length() > 524288) return false;
		TArray<uint8> Bytes;
#if PLATFORM_WINDOWS
		DATA_BLOB Plain{static_cast<DWORD>(Utf8.Length()), reinterpret_cast<BYTE*>(const_cast<ANSICHAR*>(Utf8.Get()))};
		DATA_BLOB Protected{};
		if (!CryptProtectData(&Plain, L"ACE login", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Protected)) return false;
		Bytes.Append(Protected.pbData, Protected.cbData);
		LocalFree(Protected.pbData);
#elif PLATFORM_ANDROID
		if (!AndroidCrypt(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), true, Bytes)) return false;
#elif PLATFORM_LINUX || PLATFORM_MAC
        // Password persistence is opt-in; the local file is owner-readable only.
        Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
#else
		return false;
#endif
		const FString Directory = FPaths::GetPath(Path);
		if (!IFileManager::Get().MakeDirectory(*Directory, true)) return false;
		const FString TemporaryPath = Path + TEXT(".tmp-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
#if PLATFORM_LINUX || PLATFORM_MAC
		// Restrict access when creating the file, before any plaintext is written.
		const int Fd = open(TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(TemporaryPath)),
			O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
		bool Saved = Fd >= 0;
		int32 Written = 0;
		while (Saved && Written < Bytes.Num())
		{
			const ssize_t Count = write(Fd, Bytes.GetData()+Written, Bytes.Num()-Written);
			if (Count < 0 && errno == EINTR) continue;
			if (Count <= 0) { Saved = false; break; }
			Written += static_cast<int32>(Count);
		}
		if (Fd >= 0 && close(Fd) != 0) Saved = false;
#else
		bool Saved = FFileHelper::SaveArrayToFile(Bytes, *TemporaryPath);
#endif
        Saved = Saved && IFileManager::Get().Move(*Path, *TemporaryPath, true, false);
		if (!Saved) IFileManager::Get().Delete(*TemporaryPath, false, true);
		return Saved;
	}

	bool Load(FACELoginSettings& Settings, const FString& Path)
	{
		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size <= 0 || Size > 1048576) return false;
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path)) return false;
		FString Text;
#if PLATFORM_WINDOWS
		DATA_BLOB Protected{static_cast<DWORD>(Bytes.Num()), Bytes.GetData()};
		DATA_BLOB Plain{};
		if (!CryptUnprotectData(&Protected, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Plain)) return false;
		if (Plain.cbData <= 524288)
		{
			const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Plain.pbData), Plain.cbData);
			Text = FString(Utf8.Length(), Utf8.Get());
		}
		SecureZeroMemory(Plain.pbData, Plain.cbData);
		LocalFree(Plain.pbData);
#elif PLATFORM_ANDROID
		TArray<uint8> Plain;
		if (!AndroidCrypt(Bytes.GetData(), Bytes.Num(), false, Plain)) return false;
		if (Plain.Num() <= 524288)
		{
			const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Plain.GetData()), Plain.Num());
			Text = FString(Utf8.Length(), Utf8.Get());
		}
		FMemory::Memzero(Plain.GetData(), Plain.Num());
#elif PLATFORM_LINUX || PLATFORM_MAC
        if (Bytes.Num() > 524288) return false;
        const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
        Text = FString(Utf8.Length(), Utf8.Get());
#else
		return false;
#endif
		TSharedPtr<FJsonObject> Json;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json) return false;
		FACELoginSettings Loaded;
		Json->TryGetBoolField(TEXT("savePasswordsLocally"), Loaded.bSavePasswordsLocally);
		if (!Json->TryGetStringField(TEXT("host"), Loaded.Host)
			|| !Json->TryGetStringField(TEXT("port"), Loaded.Port)
			|| !Json->TryGetStringField(TEXT("account"), Loaded.Account)
			|| !Json->TryGetStringField(TEXT("password"), Loaded.Password)) return false;
#if PLATFORM_LINUX || PLATFORM_MAC
        if (!Loaded.bSavePasswordsLocally) Loaded.Password.Reset();
#endif
		double Version = 0;
		if (Json->TryGetNumberField(TEXT("version"), Version))
		{
			if (Version != 2 && Version != 3) return false;
			Loaded.bProfileFormat = true;
			Json->TryGetStringField(TEXT("selectedServer"), Loaded.SelectedServerId);
			Json->TryGetStringField(TEXT("selectedAccount"), Loaded.SelectedAccountId);
			Json->TryGetStringField(TEXT("datDirectory"), Loaded.DatDirectory);
			// Missing on existing installations: automatic installation is opt-in.
			Json->TryGetBoolField(TEXT("autoUpdate"), Loaded.bAutoUpdate);
			const TArray<TSharedPtr<FJsonValue>> *Servers = nullptr, *Accounts = nullptr;
			if (!Json->TryGetArrayField(TEXT("servers"), Servers) || !Json->TryGetArrayField(TEXT("accounts"), Accounts)
				|| Servers->Num() > 128 || Accounts->Num() > 512) return false;
			TSet<FString> ServerIds, AccountIds;
			for (const auto& V : *Servers)
			{
				const TSharedPtr<FJsonObject>* J = nullptr; FACELoginServer S;
				if (!V->TryGetObject(J) || !(*J)->TryGetStringField(TEXT("id"), S.Id) || S.Id.IsEmpty() || ServerIds.Contains(S.Id)
					|| !(*J)->TryGetStringField(TEXT("name"), S.Name) || !(*J)->TryGetStringField(TEXT("host"), S.Host)
					|| !(*J)->TryGetNumberField(TEXT("port"), S.Port) || !(*J)->TryGetBoolField(TEXT("gdle"), S.bGDLE)) return false;
				(*J)->TryGetStringField(TEXT("description"), S.Description); (*J)->TryGetStringField(TEXT("website"), S.Website);
				(*J)->TryGetStringField(TEXT("discord"), S.Discord); FString Error;
				if (!ACELoginProfile::ValidateServer(S, Error)) return false;
				ServerIds.Add(S.Id); Loaded.Servers.Add(MoveTemp(S));
			}
			for (const auto& V : *Accounts)
			{
				const TSharedPtr<FJsonObject>* J = nullptr; FACELoginAccount A;
				if (!V->TryGetObject(J) || !(*J)->TryGetStringField(TEXT("id"), A.Id) || A.Id.IsEmpty() || AccountIds.Contains(A.Id)
					|| !(*J)->TryGetStringField(TEXT("username"), A.Username) || !(*J)->TryGetStringField(TEXT("password"), A.Password)) return false;
				// Optional for existing saved logins; identity remains the entry ID.
				(*J)->TryGetStringField(TEXT("alias"), A.Alias);
#if PLATFORM_LINUX || PLATFORM_MAC
                if (!Loaded.bSavePasswordsLocally) A.Password.Reset();
#endif
				AccountIds.Add(A.Id);
				// Upgrade any early server-scoped profile without losing alternate
				// passwords. Only exact duplicates can safely be combined.
				const auto* Duplicate = Version == 2 ? Loaded.Accounts.FindByPredicate([&](const auto& E)
					{ return E.Username == A.Username && E.Password == A.Password && E.Alias == A.Alias; }) : nullptr;
				if (Duplicate)
				{
					if (Loaded.SelectedAccountId == A.Id) Loaded.SelectedAccountId = Duplicate->Id;
				}
				else Loaded.Accounts.Add(MoveTemp(A));
			}
			if (!ServerIds.Contains(Loaded.SelectedServerId)) Loaded.SelectedServerId = Loaded.Servers.IsEmpty() ? FString() : Loaded.Servers[0].Id;
			if (!Loaded.Accounts.ContainsByPredicate([&](const auto& A) { return A.Id == Loaded.SelectedAccountId; }))
				Loaded.SelectedAccountId.Reset();
		}
		Settings = MoveTemp(Loaded);
		return true;
	}
}
