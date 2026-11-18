#include "Sky/ACELegionsSky.h"
#include "Dat/ACEDatTextureResolver.h"
#include "HAL/FileManager.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryWriter.h"

namespace ACELegionsSky
{
namespace
{
// AC2 Legions' clear-day record. AC1's server still owns the clock and weather.
constexpr uint32 ClearDay = 0x1A000002;
constexpr int32 MaxRecord = 8 * 1024 * 1024;
constexpr int32 MaxPack = 40 * 1024 * 1024;
constexpr uint32 LegacyPack = 0x314B534C; // LSK1 did not import the terrain cloud mask.
constexpr uint32 CloudPack = 0x324B534C; // LSK2 added the cloud mask.
constexpr uint32 PackVersion = 0x334B534C; // LSK3 includes the region's rain schedules.
TAtomic<int32> ImportRevision{0};
FCriticalSection CacheMutex;
using FRecords = TMap<uint32, TArray<uint8>>;

// Sticky bounds checks, including nested record counts. Little-endian AC2 DAT.
struct FReader
{
	TConstArrayView<uint8> Data;
	int32 Pos = 0;
	bool Ok = true;
	TConstArrayView<uint8> Take(int32 N)
	{
		if (!Ok || N < 0 || N > Data.Num() - Pos)
		{
			Ok = false;
			return {};
		}
		auto Result = Data.Slice(Pos, N);
		Pos += N;
		return Result;
	}
	template <typename T> T Scalar()
	{
		T Value{};
		auto Bytes = Take(sizeof(T));
		if (Ok)
			FMemory::Memcpy(&Value, Bytes.GetData(), sizeof(T));
		return Value;
	}
	uint32 Word()
	{
		return Scalar<uint32>();
	}
	float Float()
	{
		const float V = Scalar<float>();
		Ok &= FMath::IsFinite(V);
		return Ok ? V : 0.f;
	}
	int32 Count(int32 Max)
	{
		const uint32 N = Word();
		Ok &= N <= uint32(Max);
		return Ok ? int32(N) : 0;
	}
	FVector Vector()
	{
		const float X = Float(), Y = Float(), Z = Float();
		return FVector(X, Y, Z);
	}
};

struct FArchiveFile
{
	TUniquePtr<FArchive> File;
	uint32 Sector = 0, Root = 0;
	bool Open(const FString& Path)
	{
		File.Reset(IFileManager::Get().CreateFileReader(*Path, FILEREAD_AllowWrite));
		if (!File || File->TotalSize() < 380)
			return false;
		uint32 H[9]{};
		File->Seek(300);
		File->Serialize(H, sizeof(H));
		Sector = H[1];
		Root = H[8];
		return !File->IsError() && H[0] == 0x5442 && H[3] == 1 && H[2] == File->TotalSize() && Sector >= 256 &&
		       Sector <= 65536 && FMath::IsPowerOfTwo(Sector) && ValidSector(Root);
	}
	bool ValidSector(uint32 At) const
	{
		return At >= 1024 && At % Sector == 0 && int64(At) + Sector <= File->TotalSize();
	}
	bool Chain(uint32 At, uint32 Size, TArray<uint8>& Out)
	{
		if (!Size || Size > MaxRecord)
			return false;
		Out.SetNumUninitialized(Size);
		TSet<uint32> Visited;
		for (uint32 Written = 0; Written < Size;)
		{
			if (!ValidSector(At) || Visited.Contains(At))
				return false;
			Visited.Add(At);
			File->Seek(At);
			const uint32 Count = FMath::Min(Size - Written, Sector - 4);
			File->Serialize(&At, 4);
			File->Serialize(Out.GetData() + Written, Count);
			if (File->IsError())
				return false;
			Written += Count;
		}
		return At == 0;
	}
	bool Read(uint32 Id, TArray<uint8>& Out)
	{
		uint32 At = Root;
		for (int32 Depth = 0; At && Depth < 64; ++Depth)
		{
			TArray<uint8> Node;
			if (!Chain(At, 1228, Node))
				return false;
			FReader R{Node};
			R.Pos = 248;
			const int32 N = R.Count(61);
			if (!R.Ok)
				return false;
			int32 I = 0;
			for (; I < N; ++I)
			{
				const uint32 Key = R.Word(), Address = R.Word(), Size = R.Word();
				R.Word();
				if (Key == Id)
					return Chain(Address, Size, Out);
				if (Key > Id)
					break;
			}
			R.Pos = 0;
			if (!R.Word())
				break;
			R.Pos = I * 4;
			At = R.Word();
		}
		return false;
	}
};

struct FSource
{
	FArchiveFile Portal, High;
	bool HasPortal = false, HasHigh = false;
	FRecords Records;
	int32 Total = 0;
	int32 DecodedPixels = 0;
	FReader Get(uint32 Id)
	{
		if (auto* Bytes = Records.Find(Id))
			return {*Bytes};
		TArray<uint8> B;
		if (!HasPortal || Records.Num() >= 160 ||
		    !((HasHigh && (Id >> 24) == 0x41 && High.Read(Id, B)) || Portal.Read(Id, B)) ||
		    B.Num() > MaxPack - Total)
			return {{}, 0, false};
		Total += B.Num();
		return {Records.Add(Id, MoveTemp(B))};
	}
};

FLinearColor ReadColor(FReader& Value)
{
	const float Red = Value.Float(), Green = Value.Float(), Blue = Value.Float(), Alpha = Value.Float();
	return FLinearColor(Red, Green, Blue, Alpha);
}

bool ReadTexture(FSource& Src, uint32 Id, FBackdrop& Sky)
{
	if (Sky.Images.Contains(Id))
		return true;
	if (Sky.Images.Num() >= 16 || (Id >> 24) != 0x40)
		return false;
	FReader R = Src.Get(Id);
	if (R.Word() != Id)
		return false;
	R.Word();
	const int32 Levels = R.Count(32);
	FImage Image;
	int32 NextWidth = 0, NextHeight = 0;
	for (int32 I = 0; I < Levels; ++I)
	{
		const uint32 Surface = R.Word();
		if ((Surface >> 24) != 0x41)
			return false;
		FReader S = Src.Get(Surface);
		if (!S.Ok)
			continue; // Highest installed mip, highres.dat is optional.
		if (S.Word() != Surface)
			return false;
		S.Word();
		FACEDatTexture T;
		T.Width = S.Count(2048);
		T.Height = S.Count(2048);
		T.Format = EACESurfacePixelFormat(S.Word());
		const int32 Bytes = S.Count(MaxRecord);
		const auto Payload = S.Take(Bytes);
		if (!S.Ok || !T.Width || !T.Height || S.Pos != S.Data.Num())
			return false;
		if (!Image.Mips.IsEmpty() && (T.Width != NextWidth || T.Height != NextHeight))
			return false;
		Src.DecodedPixels += T.Width * T.Height;
		if (Src.DecodedPixels > 16 * 1024 * 1024)
			return false;
		// Validate sizes before handing a record to the shared AC texture decoder.
		int32 Expected = 0;
		EPixelFormat BlockFormat = PF_Unknown;
		switch (T.Format)
		{
		case EACESurfacePixelFormat::DXT1:
			BlockFormat = PF_DXT1;
			Expected = ((T.Width + 3) / 4) * ((T.Height + 3) / 4) * 8;
			break;
		case EACESurfacePixelFormat::DXT3:
		case EACESurfacePixelFormat::DXT5:
			BlockFormat = T.Format == EACESurfacePixelFormat::DXT3 ? PF_DXT3 : PF_DXT5;
			Expected = ((T.Width + 3) / 4) * ((T.Height + 3) / 4) * 16;
			break;
		case EACESurfacePixelFormat::R8G8B8:
			Expected = T.Width * T.Height * 3;
			break;
		case EACESurfacePixelFormat::A8R8G8B8:
			Expected = T.Width * T.Height * 4;
			break;
		default:
			return false;
		}
		if (Bytes != Expected)
			return false;
		if (Image.Mips.IsEmpty())
		{
			Image.Width = T.Width;
			Image.Height = T.Height;
			if (Id != 0x400003e7 && BlockFormat != PF_Unknown && GPixelFormats[BlockFormat].Supported &&
			    T.Width % 4 == 0 && T.Height % 4 == 0)
				Image.Format = BlockFormat;
		}
		if (Image.Format != PF_B8G8R8A8)
		{
			if (Image.Format != BlockFormat) return false;
			Image.Mips.Emplace(Payload);
		}
		else
		{
			T.SourceData.Append(Payload.GetData(), Bytes);
			FACEDatTextureResolver Decoder(nullptr, nullptr);
			FACEDatDecodedSurface Decoded;
			if (!Decoder.DecodeTextureForUi(T, Decoded) || Decoded.Pixels.Num() != T.Width*T.Height)
				return false;
			Image.Mips.AddDefaulted_GetRef().Append(reinterpret_cast<const uint8*>(Decoded.Pixels.GetData()), Decoded.Pixels.Num()*sizeof(FColor));
		}
		NextWidth = FMath::Max<int32>(1, T.Width / 2);
		NextHeight = FMath::Max<int32>(1, T.Height / 2);
	}
	if (!R.Ok || Image.Mips.IsEmpty())
		return false;
	// This matte encodes a vertical opacity ramp as dithered white DXT3
	// blocks. Recover the row coverage so magnification doesn't expose the grid.
	if (Id == 0x400003e7)
	{
		for (int32 Level = 0; Level < Image.Mips.Num(); ++Level)
		{
			auto& Bytes = Image.Mips[Level];
			FColor* Pixels = reinterpret_cast<FColor*>(Bytes.GetData());
			const int32 Width = FMath::Max(1, Image.Width >> Level), Height = FMath::Max(1, Image.Height >> Level);
			TArray<int64> RowSums;
			RowSums.Add(0);
			for (int32 Y = 0; Y < Height; ++Y)
			{
				int64 Sum = RowSums.Last();
				for (int32 X = 0; X < Width; ++X)
					Sum += Pixels[Y * Width + X].A;
				RowSums.Add(Sum);
			}
			const int32 Radius = FMath::Max(1, Height / 256);
			for (int32 Y = 0; Y < Height; ++Y)
			{
				const int32 First = FMath::Max(0, Y - Radius), End = FMath::Min(Height, Y + Radius + 1);
				const int32 Samples = (End - First) * Width;
				Pixels[Y] = FColor(255, 255, 255, (RowSums[End] - RowSums[First] + Samples / 2) / Samples);
			}
			Bytes.SetNum(Height*sizeof(FColor));
		}
		Image.Width = 1;
	}
	Sky.Images.Add(Id, MoveTemp(Image));
	return true;
}

bool ReadMaterial(FSource& Src, uint32 Id, FSection& Section, FBackdrop& Sky)
{
	FReader M = Src.Get(Id);
	if ((Id >> 24) != 0x31 || M.Word() != Id)
		return false;
	const uint32 Template = M.Word();
	// The clear sky uses the template's first fixed-function layer. Its blend
	// factors distinguish glowing celestial cards from alpha-blended clouds.
	FReader Base = Src.Get(Template);
	if ((Template >> 24) != 0x2b || Base.Word() != Template)
		return false;
	Base.Word();
	const int32 Defaults = Base.Count(256);
	for (int32 I = 0; I < Defaults; ++I)
	{
		const uint32 Name = Base.Word();
		Base.Word();
		FReader Value{Base.Take(Base.Count(4096))};
		if (Name == 2000)
		{
			Section.Tint = ReadColor(Value);
			if (!Value.Ok)
				return false;
		}
		Base.Take(Base.Count(256) * 16);
	}
	if (!Base.Count(64))
		return false;
	Base.Take(8);
	const int32 Stages = Base.Count(2);
	for (int32 I = 0; I < Stages; ++I)
	{
		Base.Word();
		const uint32 ColorOp = Base.Word(), ColorA = Base.Word(), ColorB = Base.Word();
		const uint32 AlphaOp = Base.Word(), AlphaA = Base.Word(), AlphaB = Base.Word();
		if (ColorA != 2 || AlphaA != 2 || ColorB != (I ? 1u : 3u) || AlphaB != ColorB)
			return false;
		if (!I)
		{
			if ((ColorOp != 4 && ColorOp != 5) || (AlphaOp != 3 && AlphaOp != 4))
				return false;
			Section.StageOps.R = ColorOp == 5 ? 2.f : 1.f;
			Section.StageOps.G = AlphaOp == 4 ? 1.f : 0.f;
		}
		else
		{
			if ((ColorOp != 3 && ColorOp != 8) || (AlphaOp != 3 && AlphaOp != 5))
				return false;
			Section.StageOps.B = ColorOp == 8 ? 1.f : 0.f;
			Section.StageOps.A = AlphaOp == 5 ? 1.f : 0.f;
		}
		Base.Take(28);
	}
	const int32 Modifiers = Base.Count(256);
	for (int32 I = 0; I < Modifiers; ++I)
	{
		const uint32 Kind = Base.Word();
		Base.Word();
		switch (Kind)
		{
		case 4:
		case 6:
			Base.Take(88);
			break;
		case 5:
			Base.Take(44);
			break;
		case 7:
			Base.Take(64);
			break;
		default:
			return false;
		}
	}
	const uint32 SourceBlend = Base.Word(), DestBlend = Base.Word();
	if (!Base.Ok || SourceBlend != 5 || (DestBlend != 2 && DestBlend != 6))
		return false;
	Section.Additive = DestBlend == 2;
	M.Word();
	const int32 Mods = M.Count(8);
	uint32 SecondaryTexture = 0;
	for (int32 I = 0; I < Mods; ++I)
	{
		const uint32 Mod = M.Word();
		FReader R = Src.Get(Mod);
		if ((Mod >> 24) != 0x30 || R.Word() != Mod)
			return false;
		const int32 Props = R.Count(64);
		for (int32 J = 0; J < Props; ++J)
		{
			const uint32 Name = R.Word();
			R.Word();
			const int32 Size = R.Count(4096);
			FReader Value{R.Take(Size)};
			if (Name == 3000 && Size == 4)
				Section.Texture = Value.Word();
			if (Name == 3001 && Size == 4)
				SecondaryTexture = Value.Word();
			if (Name == 2000 && Size == 16)
				Section.Tint = ReadColor(Value);
			// Material UV panners use elapsed seconds. The clear sky uses linear phase waves.
			if (Name >= 1017 && Name <= 1020 && Size == 44 && Value.Word() == 2)
			{
				const int32 Stage = (Name - 1017) / 2, Axis = (Name - 1017) % 2;
				const float BaseValue = Value.Float(), BaseSpeed = Value.Float();
				Value.Take(8);
				const float Phase = Value.Float(), PhaseSpeed = Value.Float(), Frequency = Value.Float();
				Section.Offset[Stage][Axis] = BaseValue + Phase * UE_PI;
				Section.Velocity[Stage][Axis] = BaseSpeed + PhaseSpeed + Frequency;
			}
			if (!Value.Ok)
				return false;
			R.Take(R.Count(64) * 16);
		}
		if (!R.Ok || R.Pos != R.Data.Num())
			return false;
	}
	return M.Ok && Stages > 0 && (Stages == 1 || SecondaryTexture == Section.Texture) &&
	       ReadTexture(Src, Section.Texture, Sky);
}

bool ReadGeometry(FSource& Src, uint32 Setup, FLayer& Layer, FBackdrop& Sky)
{
	FReader S = Src.Get(Setup);
	if ((Setup >> 24) != 4 || S.Word() != Setup)
		return false;
	S.Word();
	if (S.Word() != 1)
		return false;
	const uint32 Mesh = S.Word();
	// All native clear-day setups are single rigid meshes, with one placement frame.
	if (S.Word() != 1 || S.Word() != MAX_uint32)
		return false;
	S.Word();
	if (S.Count(32) < 1)
		return false;
	S.Word();
	if (S.Word() != 1)
		return false;
	const FVector Origin = S.Vector(), Scale = S.Vector();
	const float W = S.Float(), X = S.Float(), Y = S.Float(), Z = S.Float();
	const FQuat Rotation(X, Y, Z, W);
	if (!S.Ok || !Rotation.IsNormalized())
		return false;
	FReader R = Src.Get(Mesh);
	if ((Mesh >> 24) != 6 || R.Word() != Mesh)
		return false;
	R.Word();
	const int32 Materials = R.Count(8);
	TArray<uint32> MaterialIds;
	for (int32 I = 0; I < Materials; ++I)
		MaterialIds.Add(R.Word());
	const uint32 FVF = R.Word();
	// Position, normal, diffuse, specular (two-UV variants include tangents).
	if (FVF != 0x1d2 && FVF != 0x300002d2)
		return false;
	const int32 NV = R.Count(4096), Stride = FVF == 0x1d2 ? 40 : 72;
	const int32 UVOffset = FVF == 0x1d2 ? 32 : 56;
	double Nearest = DBL_MAX;
	for (int32 I = 0; I < NV; ++I)
	{
		FReader V{R.Take(Stride)};
		const FVector P = Rotation.RotateVector(Scale * V.Vector()) + Origin;
		V.Pos = UVOffset;
		const float U = V.Float(), TexV = V.Float();
		if (!V.Ok || P.ContainsNaN() || P.GetAbsMax() > 1.e6)
			return false;
		Layer.Vertices.Add(P);
		Layer.UVs.Emplace(U, TexV);
		if (P.Size() > 0.01)
			Nearest = FMath::Min(Nearest, P.Size());
	}
	if (Nearest == DBL_MAX)
		return false;
	for (auto& P : Layer.Vertices)
		P *= 1.e7 / Nearest;
	Layer.Center = FBox(Layer.Vertices).GetCenter();
	R.Take(40);
	const int32 Draws = R.Count(32);
	bool Preferred = false;
	for (int32 I = 0; I < Draws; ++I)
	{
		R.Word();
		const uint32 Flags = R.Word();
		const int32 NF = R.Count(16);
		TArray<FSection> Sections;
		const bool Keep = I == 0 || (!Preferred && (Flags & 4));
		for (int32 J = 0; J < NF; ++J)
		{
			const int32 Slot = R.Count(8), NI = R.Count(50000);
			if (!MaterialIds.IsValidIndex(Slot) || NI % 3)
				return false;
			FReader Indices{R.Take(2 * NI)};
			R.Take((4 - (R.Pos & 3)) & 3);
			if (!Keep)
				continue;
			FSection& Section = Sections.AddDefaulted_GetRef();
			for (int32 K = 0; K < NI; K += 3)
			{
				const int32 A = Indices.Scalar<uint16>(), B = Indices.Scalar<uint16>(),
				            C = Indices.Scalar<uint16>();
				if (FMath::Max3(A, B, C) >= NV)
					return false;
				Section.Indices.Append({A, C, B});
			}
			if (!Indices.Ok || !ReadMaterial(Src, MaterialIds[Slot], Section, Sky))
				return false;
		}
		if (Keep)
		{
			Layer.Sections = MoveTemp(Sections);
			Preferred = (Flags & 4) != 0;
		}
	}
	return R.Ok && !Layer.Sections.IsEmpty();
}

bool ReadDayKeys(FReader& R, uint32 DayId, TArray<FKey>& Keys)
{
	R.Word();
	if (R.Word() != DayId)
		return false;
	const int32 NK = R.Count(64);
	for (int32 K = 0; K < NK; ++K)
	{
		const float Time = R.Float();
		if (Time < 0 || Time > 1 || (K && Time <= Keys.Last().Time))
			return false;
		auto& Key = Keys.AddDefaulted_GetRef();
		Key.Time = Time;
		Key.FogMin = R.Float();
		Key.FogMax = R.Float();
		const float Haze = R.Float();
		Key.Haze = FColor(R.Word()).ReinterpretAsLinear();
		Key.Haze.A = FMath::Clamp(Haze, 0.f, 1.f);
		Key.Direct = R.Float();
		Key.Heading = R.Float();
		Key.Pitch = R.Float();
		Key.DirectColor = FColor(R.Word()).ReinterpretAsLinear();
		Key.Ambient = R.Float();
		Key.AmbientColor = FColor(R.Word()).ReinterpretAsLinear();
		R.Word();
		const int32 N = R.Count(16);
		for (int32 I = 0; I < N; ++I)
		{
			const uint32 Name = R.Word();
			if (R.Word() != 0)
				return false; // This preset tints and rotates its own graphics.
			const float Pitch = R.Float(), Heading = R.Float();
			Key.Objects.Add(Name, {Pitch, Heading, FColor(R.Word()).ReinterpretAsLinear()});
		}
		Key.Rain = R.Float();
		R.Take(8); // wind vector
	}
	return R.Ok && NK >= 2 && Keys[0].Time == 0 && Keys.Last().Time == 1;
}

bool Decode(FSource& Src, FBackdrop& Sky, bool WithCloudMask, bool WithWeather)
{
	if (WithCloudMask)
	{
		FReader Surface = Src.Get(0x13000000);
		Surface.Word();
		if (Surface.Word() != 0x13000000)
			return false;
		Surface.Take(12);
		Sky.CloudMask = Surface.Word();
		const float Scale = Surface.Float(), U = Surface.Float(), V = Surface.Float();
		Sky.CloudMotion = FLinearColor(Scale, U, V, 0);
		if (!Surface.Ok || !ReadTexture(Src, Sky.CloudMask, Sky))
			return false;
	}
	FReader R = Src.Get(ClearDay);
	if (!ReadDayKeys(R, ClearDay, Sky.Keys))
		return false;
	if (WithWeather)
	{
		// Legions SkyDesc lists the early shower, daytime shower and clear day.
		// Ignore unreferenced test days in portal.dat.
		FReader Desc = Src.Get(0x15000000);
		Desc.Word();
		if (Desc.Word() != 0x15000000)
			return false;
		Desc.Take(36);
		const int32 Count = Desc.Count(16);
		const auto Objects = R.Data.RightChop(R.Pos);
		for (int32 I = 0; I < Count; ++I)
		{
			const uint32 DayId = Desc.Word();
			const float Weight = Desc.Float();
			if (DayId == ClearDay || Weight <= 0)
				continue;
			auto& Day = Sky.RainDays.AddDefaulted_GetRef();
			FReader Rain = Src.Get(DayId);
			// These days share one object list. Do not silently apply a schedule
			// to incompatible geometry if a different archive is selected.
			if (!ReadDayKeys(Rain, DayId, Day.Keys) || Rain.Data.Num() - Rain.Pos != Objects.Num() ||
			    FMemory::Memcmp(Rain.Data.GetData() + Rain.Pos, Objects.GetData(), Objects.Num()) != 0)
				return false;
			for (int32 K = 1; K < Day.Keys.Num(); ++K)
				if (Day.Keys[K].Rain > Day.Keys[Day.Wet].Rain)
					Day.Wet = K;
			if (Day.Keys[Day.Wet].Rain <= 0)
				return false;
			Day.Cloudy = Day.Wet;
			while (Day.Cloudy > 0 && Day.Keys[Day.Cloudy].Rain > 0)
				--Day.Cloudy;
		}
		if (!Desc.Ok || Desc.Pos != Desc.Data.Num() || Sky.RainDays.IsEmpty())
			return false;
	}
	const int32 NO = R.Count(16);
	for (int32 I = 0; I < NO; ++I)
	{
		FLayer& L = Sky.Layers.AddDefaulted_GetRef();
		L.Name = R.Word();
		L.Setup = R.Word();
		L.Begin = R.Float();
		L.End = R.Float();
		L.BeginPitch = R.Float();
		L.EndPitch = R.Float();
		L.BeginHeading = R.Float();
		L.EndHeading = R.Float();
		R.Take(24);
		L.West = R.Word() != 0;
		R.Float();
		R.Take(R.Count(64) * 12);
		if (!R.Ok || !ReadGeometry(Src, L.Setup, L, Sky))
			return false;
	}
	return R.Ok && R.Pos == R.Data.Num() && NO > 0;
}
}

FString CachePath()
{
	return FPaths::ProjectSavedDir() / TEXT("Skies/Legions.sky");
}
bool HasCache(bool* NeedsWeather)
{
	FScopeLock Lock(&CacheMutex);
	const FString Path = CachePath();
	auto& Files = IFileManager::Get();
	const FString Actual = Files.FileExists(*Path) ? Path : Path + TEXT(".previous");
	if (NeedsWeather)
	{
		*NeedsWeather = false;
		TUniquePtr<FArchive> Reader(Files.CreateFileReader(*Actual));
		uint32 Version = 0;
		if (!Reader || Reader->TotalSize()<sizeof(Version)) return false;
		*Reader << Version;
		*NeedsWeather = Version == LegacyPack || Version == CloudPack;
		return !Reader->IsError() && (Version == PackVersion || Version == CloudPack || Version == LegacyPack);
	}
	return Files.FileExists(*Actual);
}
int32 Revision()
{
	return ImportRevision.Load();
}

namespace
{
bool PublishCache(const FString& Incoming, const FString& Destination, FString& Error)
{
	FScopeLock Lock(&CacheMutex);
	auto& Files = IFileManager::Get();
	const FString Previous = Destination + TEXT(".previous");
	if ((Files.FileExists(*Destination) && !Files.Move(*Previous, *Destination, true)) ||
	    !Files.Move(*Destination, *Incoming, false))
	{
		if (!Files.FileExists(*Destination) && Files.FileExists(*Previous))
			Files.Move(*Destination, *Previous, false);
		Error = TEXT("The imported sky could not be saved.");
		return false;
	}
	Files.Delete(*Previous, false);
	++ImportRevision;
	Error.Reset();
	return true;
}

bool ImportArchives(const FString& Portal, const FString& HighRes, const FString& Destination, FString& Error)
{
	FSource Src;
	Src.HasPortal = Src.Portal.Open(Portal);
	if (!Src.HasPortal)
	{
		Error = TEXT("Choose an AC2 Legions folder containing portal.dat.");
		return false;
	}
	Src.HasHigh = !HighRes.IsEmpty() && Src.High.Open(HighRes);
	FBackdrop Sky;
	if (!Decode(Src, Sky, true, true))
	{
		Error = TEXT("These DATs do not contain a supported Legions sky and weather.");
		return false;
	}

	TArray<uint8> Pack;
	FMemoryWriter Writer(Pack);
	uint32 Version = PackVersion, Count = Src.Records.Num();
	Writer << Version << Count;
	for (auto& Pair : Src.Records)
	{
		uint32 Id = Pair.Key;
		int32 Size = Pair.Value.Num();
		Writer << Id << Size;
		Writer.Serialize(Pair.Value.GetData(), Size);
	}
	uint32 Crc = FCrc::MemCrc32(Pack.GetData(), Pack.Num());
	Writer << Crc;
	auto& Files = IFileManager::Get();
	const FString Dir = FPaths::GetPath(Destination);
	Files.MakeDirectory(*Dir, true);
	const FString Temp = FPaths::CreateTempFilename(*Dir, TEXT("legions-"), TEXT(".tmp"));
	if (FFileHelper::SaveArrayToFile(Pack, *Temp) && PublishCache(Temp, Destination, Error))
		return true;
	Files.Delete(*Temp, false, true);
	Error = TEXT("The imported sky could not be saved.");
	return false;
}
}

bool Import(const FString& DirectoryOrFile, const FString& Destination, FString& Error)
{
	FString Folder = DirectoryOrFile.TrimStartAndEnd().TrimQuotes();
	FPaths::NormalizeDirectoryName(Folder);
	if (FPaths::GetCleanFilename(Folder).Equals(TEXT("portal.dat"), ESearchCase::IgnoreCase))
		Folder = FPaths::GetPath(Folder);
	if (Folder.IsEmpty())
	{
		Error = TEXT("Choose an AC2 Legions folder containing portal.dat.");
		return false;
	}
	return ImportArchives(Folder / TEXT("portal.dat"), Folder / TEXT("highres.dat"), Destination, Error);
}

FString StagedPath(const FString& Destination)
{
	return FPaths::GetPath(Destination) / TEXT("Import/Legions.source");
}

bool ImportStaged(const FString& Destination, FString& Error)
{
	auto& Files = IFileManager::Get();
	const FString Source = StagedPath(Destination);
	uint32 Version = 0;
	{
		TUniquePtr<FArchive> Reader(Files.CreateFileReader(*Source));
		if (!Reader || Reader->TotalSize() < sizeof(Version))
		{
			Error = TEXT("Transfer your AC2 sky file from the PC again.");
			return false;
		}
		*Reader << Version;
	}
	if (Version == PackVersion || Version == CloudPack || Version == LegacyPack)
	{
		FBackdrop Sky;
		if (!Load(Source, Sky, Error))
		{
			Error = TEXT("Invalid sky cache. Import your AC2 portal.dat or transfer a fresh Legions.sky.");
			return false;
		}
		return PublishCache(Source, Destination, Error);
	}
	if (!ImportArchives(Source, FString(), Destination, Error))
		return false;
	if (!Files.Delete(*Source))
		UE_LOG(LogTemp, Warning, TEXT("AC2 sky installed, but its transfer file could not be removed."));
	return true;
}

bool Load(const FString& Filename, FBackdrop& Sky, FString& Error)
{
	Sky = {};
	Error = TEXT("Import the AC2 Legions sky in Graphics Options.");
	TArray<uint8> Pack;
	{
		FScopeLock Lock(&CacheMutex);
		const FString Path = IFileManager::Get().FileExists(*Filename) ? Filename : Filename + TEXT(".previous");
		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size < 12 || Size > MaxPack + 2048 || !FFileHelper::LoadFileToArray(Pack, *Path) || Pack.Num() != Size)
			return false;
	}
	FReader R{Pack};
	R.Pos = Pack.Num() - 4;
	const uint32 ExpectedCrc = FCrc::MemCrc32(Pack.GetData(), R.Pos);
	if (ExpectedCrc != R.Word())
		return false;
	R.Pos = 0;
	const uint32 Version = R.Word();
	if (Version != PackVersion && Version != CloudPack && Version != LegacyPack)
		return false;
	FSource Src;
	const int32 N = R.Count(160);
	for (int32 I = 0; I < N; ++I)
	{
		const uint32 Id = R.Word();
		const auto Bytes = R.Take(R.Count(MaxRecord));
		if (!R.Ok || Src.Records.Contains(Id))
			return false;
		Src.Records.Add(Id, TArray<uint8>(Bytes));
	}
	if (!R.Ok || R.Pos != Pack.Num() - 4 || !Decode(Src, Sky, Version != LegacyPack, Version == PackVersion))
		return false;
	Error.Reset();
	return true;
}
}
