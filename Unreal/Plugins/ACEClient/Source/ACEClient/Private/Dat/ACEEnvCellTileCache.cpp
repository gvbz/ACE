#include "Dat/ACEEnvCellTileCache.h"
#include "Dat/ACEDiskTileCache.h"
#include "HAL/FileManager.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Serialization/MemoryReader.h"

namespace ACEEnvCellTileCache
{
	namespace
	{
		template <typename T>
		void WritePod(TArray<uint8>& Buf, const T& Value)
		{
			const int32 At = Buf.AddUninitialized(sizeof(T));
			FMemory::Memcpy(Buf.GetData() + At, &Value, sizeof(T));
		}

		template <typename T>
		bool ReadPod(FMemoryReader& Ar, T& Out)
		{
			if (Ar.IsError() || Ar.TotalSize() - Ar.Tell() < static_cast<int64>(sizeof(T)))
			{
				return false;
			}
			Ar.Serialize(&Out, sizeof(T));
			return !Ar.IsError();
		}

		void WriteF32Vec3(TArray<uint8>& Buf, const FVector& V)
		{
			const float X = static_cast<float>(V.X);
			const float Y = static_cast<float>(V.Y);
			const float Z = static_cast<float>(V.Z);
			WritePod(Buf, X);
			WritePod(Buf, Y);
			WritePod(Buf, Z);
		}

		bool ReadF32Vec3(FMemoryReader& Ar, FVector& Out)
		{
			float X = 0.f, Y = 0.f, Z = 0.f;
			if (!ReadPod(Ar, X) || !ReadPod(Ar, Y) || !ReadPod(Ar, Z))
			{
				return false;
			}
			Out = FVector(X, Y, Z);
			return true;
		}

		void WriteF32Vec2(TArray<uint8>& Buf, const FVector2D& V)
		{
			const float X = static_cast<float>(V.X);
			const float Y = static_cast<float>(V.Y);
			WritePod(Buf, X);
			WritePod(Buf, Y);
		}

		bool ReadF32Vec2(FMemoryReader& Ar, FVector2D& Out)
		{
			float X = 0.f, Y = 0.f;
			if (!ReadPod(Ar, X) || !ReadPod(Ar, Y))
			{
				return false;
			}
			Out = FVector2D(X, Y);
			return true;
		}

		void WriteLinearColor(TArray<uint8>& Buf, const FLinearColor& C)
		{
			WritePod(Buf, C.R);
			WritePod(Buf, C.G);
			WritePod(Buf, C.B);
			WritePod(Buf, C.A);
		}

		bool ReadLinearColor(FMemoryReader& Ar, FLinearColor& Out)
		{
			return ReadPod(Ar, Out.R) && ReadPod(Ar, Out.G) && ReadPod(Ar, Out.B) && ReadPod(Ar, Out.A);
		}

		bool WriteMeshSection(TArray<uint8>& Buf, const FACEBuiltMeshSection& Sec)
		{
			WritePod(Buf, Sec.SurfaceId);
			WritePod(Buf, static_cast<uint8>(Sec.bClipMap ? 1 : 0));
			WritePod(Buf, static_cast<uint8>(Sec.bFullyTransparent ? 1 : 0));
			WritePod(Buf, static_cast<uint8>(Sec.bCollisionOnly ? 1 : 0));
			WritePod(Buf, static_cast<uint8>(Sec.bWrapTexture ? 1 : 0));
			WritePod(Buf, static_cast<uint32>(Sec.Vertices.Num()));
			for (const FVector& V : Sec.Vertices)
			{
				WriteF32Vec3(Buf, V);
			}
			WritePod(Buf, static_cast<uint32>(Sec.Triangles.Num()));
			for (int32 T : Sec.Triangles)
			{
				WritePod(Buf, T);
			}
			WritePod(Buf, static_cast<uint32>(Sec.Normals.Num()));
			for (const FVector& N : Sec.Normals)
			{
				WriteF32Vec3(Buf, N);
			}
			WritePod(Buf, static_cast<uint32>(Sec.UVs.Num()));
			for (const FVector2D& UV : Sec.UVs)
			{
				WriteF32Vec2(Buf, UV);
			}
			WritePod(Buf, static_cast<uint32>(Sec.VertexColors.Num()));
			for (const FLinearColor& C : Sec.VertexColors)
			{
				WriteLinearColor(Buf, C);
			}
			return true;
		}

		bool ReadMeshSection(FMemoryReader& Ar, FACEBuiltMeshSection& Sec)
		{
			Sec = FACEBuiltMeshSection();
			uint8 Clip = 0, Trans = 0, Coll = 0, Wrap = 0;
			uint32 N = 0;
			if (!ReadPod(Ar, Sec.SurfaceId) || !ReadPod(Ar, Clip) || !ReadPod(Ar, Trans) || !ReadPod(Ar, Coll)
				|| !ReadPod(Ar, Wrap) || !ReadPod(Ar, N) || N > 200000u)
			{
				return false;
			}
			Sec.bClipMap = Clip != 0;
			Sec.bFullyTransparent = Trans != 0;
			Sec.bCollisionOnly = Coll != 0;
			Sec.bWrapTexture = Wrap != 0;
			Sec.Vertices.SetNum(static_cast<int32>(N));
			for (FVector& V : Sec.Vertices)
			{
				if (!ReadF32Vec3(Ar, V))
				{
					return false;
				}
			}
			if (!ReadPod(Ar, N) || N > 600000u)
			{
				return false;
			}
			Sec.Triangles.SetNum(static_cast<int32>(N));
			for (int32& T : Sec.Triangles)
			{
				if (!ReadPod(Ar, T))
				{
					return false;
				}
			}
			if (!ReadPod(Ar, N) || N > 200000u)
			{
				return false;
			}
			Sec.Normals.SetNum(static_cast<int32>(N));
			for (FVector& Nr : Sec.Normals)
			{
				if (!ReadF32Vec3(Ar, Nr))
				{
					return false;
				}
			}
			if (!ReadPod(Ar, N) || N > 200000u)
			{
				return false;
			}
			Sec.UVs.SetNum(static_cast<int32>(N));
			for (FVector2D& UV : Sec.UVs)
			{
				if (!ReadF32Vec2(Ar, UV))
				{
					return false;
				}
			}
			if (!ReadPod(Ar, N) || N > 200000u)
			{
				return false;
			}
			Sec.VertexColors.SetNum(static_cast<int32>(N));
			for (FLinearColor& C : Sec.VertexColors)
			{
				if (!ReadLinearColor(Ar, C))
				{
					return false;
				}
			}
			return !Ar.IsError();
		}

		bool WriteSectionArray(TArray<uint8>& Buf, const TArray<FACEBuiltMeshSection>& Sections)
		{
			WritePod(Buf, static_cast<uint32>(Sections.Num()));
			for (const FACEBuiltMeshSection& Sec : Sections)
			{
				WriteMeshSection(Buf, Sec);
			}
			return true;
		}

		bool ReadSectionArray(FMemoryReader& Ar, TArray<FACEBuiltMeshSection>& Sections)
		{
			uint32 Count = 0;
			if (!ReadPod(Ar, Count) || Count > 4096u)
			{
				return false;
			}
			Sections.SetNum(static_cast<int32>(Count));
			for (FACEBuiltMeshSection& Sec : Sections)
			{
				if (!ReadMeshSection(Ar, Sec))
				{
					return false;
				}
			}
			return true;
		}

		bool SerializePayload(TArray<uint8>& Buf, const FACEBuiltEnvCellMesh& Mesh)
		{
			WritePod(Buf, Mesh.EnvCellId);
			WritePod(Buf, Mesh.Origin.X);
			WritePod(Buf, Mesh.Origin.Y);
			WritePod(Buf, Mesh.Origin.Z);
			WritePod(Buf, Mesh.Orientation.X);
			WritePod(Buf, Mesh.Orientation.Y);
			WritePod(Buf, Mesh.Orientation.Z);
			WritePod(Buf, Mesh.Orientation.W);
			WritePod(Buf, static_cast<uint32>(Mesh.Flags));
			WriteSectionArray(Buf, Mesh.Sections);
			WriteSectionArray(Buf, Mesh.CollisionSections);

			WritePod(Buf, static_cast<uint32>(Mesh.StaticObjects.Num()));
			for (const FACEDatStab& S : Mesh.StaticObjects)
			{
				WritePod(Buf, S.Id);
				WritePod(Buf, S.Origin.X);
				WritePod(Buf, S.Origin.Y);
				WritePod(Buf, S.Origin.Z);
				WritePod(Buf, S.Orientation.X);
				WritePod(Buf, S.Orientation.Y);
				WritePod(Buf, S.Orientation.Z);
				WritePod(Buf, S.Orientation.W);
			}

			WritePod(Buf, static_cast<uint32>(Mesh.PointLights.Num()));
			for (const auto& Light : Mesh.PointLights)
			{
				WriteF32Vec3(Buf, Light.BlockPosition);
				WriteLinearColor(Buf, Light.Color);
				WritePod(Buf, Light.Intensity);
				WritePod(Buf, Light.Radius);
			}
			WritePod(Buf, static_cast<uint32>(Mesh.VisibleCells.Num()));
			for (uint16 V : Mesh.VisibleCells)
			{
				WritePod(Buf, V);
			}

			WritePod(Buf, static_cast<uint32>(Mesh.CellPortals.Num()));
			for (const FACEDatCellPortal& P : Mesh.CellPortals)
			{
				WritePod(Buf, P.Flags);
				WritePod(Buf, P.PolygonId);
				WritePod(Buf, P.OtherCellId);
				WritePod(Buf, P.OtherPortalId);
			}

			WritePod(Buf, static_cast<uint32>(Mesh.PortalApertureLocalVerts.Num()));
			for (const TArray<FVector>& Poly : Mesh.PortalApertureLocalVerts)
			{
				WritePod(Buf, static_cast<uint32>(Poly.Num()));
				for (const FVector& V : Poly)
				{
					WriteF32Vec3(Buf, V);
				}
			}
			WritePod(Buf, static_cast<uint32>(Mesh.PortalApertureLocalNormals.Num()));
			for (const FVector& N : Mesh.PortalApertureLocalNormals)
			{
				WriteF32Vec3(Buf, N);
			}
			WritePod(Buf, static_cast<uint32>(Mesh.PortalApertureLocalD.Num()));
			for (float D : Mesh.PortalApertureLocalD)
			{
				WritePod(Buf, D);
			}

			WritePod(Buf, static_cast<uint32>(Mesh.OutsidePortalPlanes.Num()));
			for (const FVector4f& P : Mesh.OutsidePortalPlanes)
			{
				WritePod(Buf, P.X);
				WritePod(Buf, P.Y);
				WritePod(Buf, P.Z);
				WritePod(Buf, P.W);
			}

			WriteMeshSection(Buf, Mesh.OutsidePortalMesh);
			WritePod(Buf, static_cast<uint8>(Mesh.bPortalConnector ? 1 : 0));
			WritePod(Buf, static_cast<uint8>(Mesh.bHasLocalBounds ? 1 : 0));
			WriteF32Vec3(Buf, Mesh.LocalBoundsMin);
			WriteF32Vec3(Buf, Mesh.LocalBoundsMax);

			WritePod(Buf, static_cast<uint32>(Mesh.CellBspNodes.Num()));
			for (const FACEDatCellBspNode& Node : Mesh.CellBspNodes)
			{
				WritePod(Buf, static_cast<uint8>(Node.bLeaf ? 1 : 0));
				WritePod(Buf, Node.Plane.X);
				WritePod(Buf, Node.Plane.Y);
				WritePod(Buf, Node.Plane.Z);
				WritePod(Buf, Node.Plane.W);
				WritePod(Buf, Node.PosChild);
				WritePod(Buf, Node.NegChild);
			}
			return true;
		}

		bool DeserializePayload(FMemoryReader& Ar, FACEBuiltEnvCellMesh& Out)
		{
			Out = FACEBuiltEnvCellMesh();
			if (!ReadPod(Ar, Out.EnvCellId)
				|| !ReadPod(Ar, Out.Origin.X) || !ReadPod(Ar, Out.Origin.Y) || !ReadPod(Ar, Out.Origin.Z)
				|| !ReadPod(Ar, Out.Orientation.X) || !ReadPod(Ar, Out.Orientation.Y)
				|| !ReadPod(Ar, Out.Orientation.Z) || !ReadPod(Ar, Out.Orientation.W))
			{
				return false;
			}
			uint32 FlagsRaw = 0;
			if (!ReadPod(Ar, FlagsRaw))
			{
				return false;
			}
			Out.Flags = static_cast<EACEEnvCellFlags>(FlagsRaw);
			if (!ReadSectionArray(Ar, Out.Sections) || !ReadSectionArray(Ar, Out.CollisionSections))
			{
				return false;
			}

			uint32 Count = 0;
			if (!ReadPod(Ar, Count) || Count > 10000u)
			{
				return false;
			}
			Out.StaticObjects.SetNum(static_cast<int32>(Count));
			for (FACEDatStab& S : Out.StaticObjects)
			{
				if (!ReadPod(Ar, S.Id)
					|| !ReadPod(Ar, S.Origin.X) || !ReadPod(Ar, S.Origin.Y) || !ReadPod(Ar, S.Origin.Z)
					|| !ReadPod(Ar, S.Orientation.X) || !ReadPod(Ar, S.Orientation.Y)
					|| !ReadPod(Ar, S.Orientation.Z) || !ReadPod(Ar, S.Orientation.W))
				{
					return false;
				}
			}

			if (!ReadPod(Ar, Count) || Count > 10000u)
			{
				return false;
			}
			Out.PointLights.SetNum(static_cast<int32>(Count));
			for (auto& Light : Out.PointLights)
			{
				if (!ReadF32Vec3(Ar, Light.BlockPosition) || !ReadLinearColor(Ar, Light.Color)
					|| !ReadPod(Ar, Light.Intensity) || !ReadPod(Ar, Light.Radius)) return false;
			}
			if (!ReadPod(Ar, Count) || Count > 10000u) return false;
			Out.VisibleCells.SetNum(static_cast<int32>(Count));
			for (uint16& V : Out.VisibleCells)
			{
				if (!ReadPod(Ar, V))
				{
					return false;
				}
			}

			if (!ReadPod(Ar, Count) || Count > 10000u)
			{
				return false;
			}
			Out.CellPortals.SetNum(static_cast<int32>(Count));
			for (FACEDatCellPortal& P : Out.CellPortals)
			{
				if (!ReadPod(Ar, P.Flags) || !ReadPod(Ar, P.PolygonId)
					|| !ReadPod(Ar, P.OtherCellId) || !ReadPod(Ar, P.OtherPortalId))
				{
					return false;
				}
			}

			if (!ReadPod(Ar, Count) || Count > 10000u)
			{
				return false;
			}
			Out.PortalApertureLocalVerts.SetNum(static_cast<int32>(Count));
			for (TArray<FVector>& Poly : Out.PortalApertureLocalVerts)
			{
				uint32 VertCount = 0;
				if (!ReadPod(Ar, VertCount) || VertCount > 256u)
				{
					return false;
				}
				Poly.SetNum(static_cast<int32>(VertCount));
				for (FVector& V : Poly)
				{
					if (!ReadF32Vec3(Ar, V))
					{
						return false;
					}
				}
			}
			if (!ReadPod(Ar, Count) || Count > 10000u)
			{
				return false;
			}
			Out.PortalApertureLocalNormals.SetNum(static_cast<int32>(Count));
			for (FVector& N : Out.PortalApertureLocalNormals)
			{
				if (!ReadF32Vec3(Ar, N))
				{
					return false;
				}
			}
			if (!ReadPod(Ar, Count) || Count > 10000u)
			{
				return false;
			}
			Out.PortalApertureLocalD.SetNum(static_cast<int32>(Count));
			for (float& D : Out.PortalApertureLocalD)
			{
				if (!ReadPod(Ar, D))
				{
					return false;
				}
			}

			if (!ReadPod(Ar, Count) || Count > 10000u)
			{
				return false;
			}
			Out.OutsidePortalPlanes.SetNum(static_cast<int32>(Count));
			for (FVector4f& P : Out.OutsidePortalPlanes)
			{
				if (!ReadPod(Ar, P.X) || !ReadPod(Ar, P.Y) || !ReadPod(Ar, P.Z) || !ReadPod(Ar, P.W))
				{
					return false;
				}
			}

			if (!ReadMeshSection(Ar, Out.OutsidePortalMesh))
			{
				return false;
			}
			uint8 PortalConn = 0, HasBounds = 0;
			if (!ReadPod(Ar, PortalConn) || !ReadPod(Ar, HasBounds)
				|| !ReadF32Vec3(Ar, Out.LocalBoundsMin) || !ReadF32Vec3(Ar, Out.LocalBoundsMax))
			{
				return false;
			}
			Out.bPortalConnector = PortalConn != 0;
			Out.bHasLocalBounds = HasBounds != 0;

			if (!ReadPod(Ar, Count) || Count > 100000u)
			{
				return false;
			}
			Out.CellBspNodes.SetNum(static_cast<int32>(Count));
			for (FACEDatCellBspNode& Node : Out.CellBspNodes)
			{
				uint8 Leaf = 0;
				if (!ReadPod(Ar, Leaf)
					|| !ReadPod(Ar, Node.Plane.X) || !ReadPod(Ar, Node.Plane.Y)
					|| !ReadPod(Ar, Node.Plane.Z) || !ReadPod(Ar, Node.Plane.W)
					|| !ReadPod(Ar, Node.PosChild) || !ReadPod(Ar, Node.NegChild))
				{
					return false;
				}
				Node.bLeaf = Leaf != 0;
			}
			return !Ar.IsError();
		}
	}

	FString MakeCacheFilePath(uint32 EnvCellId, float WorldScale, uint64 DatFingerprint)
	{
		const uint32 ScaleBits = *reinterpret_cast<const uint32*>(&WorldScale);
		return ACEDiskTileCache::GetEnvCellsDir()
			/ FString::Printf(TEXT("%08X_%08X_%016llX.acec"), EnvCellId, ScaleBits, DatFingerprint);
	}

	bool Save(const FString& Path, const FACEBuiltEnvCellMesh& Mesh, float WorldScale, uint64 DatFingerprint)
	{
		TArray<uint8> Payload;
		Payload.Reserve(128 * 1024);
		if (!SerializePayload(Payload, Mesh))
		{
			return false;
		}
		TArray<uint8> Compressed;
		Compressed.AddUninitialized(FCompression::CompressMemoryBound(NAME_Zlib, Payload.Num()));
		int32 CompressedSize = Compressed.Num();
		if (!FCompression::CompressMemory(NAME_Zlib, Compressed.GetData(), CompressedSize, Payload.GetData(), Payload.Num()))
		{
			return false;
		}
		Compressed.SetNum(CompressedSize);

		TArray<uint8> FileBytes;
		WritePod(FileBytes, Magic);
		WritePod(FileBytes, SchemaVersion);
		WritePod(FileBytes, Mesh.EnvCellId);
		const uint32 ScaleBits = *reinterpret_cast<const uint32*>(&WorldScale);
		WritePod(FileBytes, ScaleBits);
		WritePod(FileBytes, DatFingerprint);
		WritePod(FileBytes, static_cast<uint32>(Payload.Num()));
		WritePod(FileBytes, static_cast<uint32>(Compressed.Num()));
		FileBytes.Append(Compressed);

		const FString TempPath = FString::Printf(TEXT("%s.%u.tmp"), *Path, FPlatformProcess::GetCurrentProcessId());
		if (!FFileHelper::SaveArrayToFile(FileBytes, *TempPath))
		{
			return false;
		}
		IFileManager& FM = IFileManager::Get();
		if (!FM.Move(*Path, *TempPath, true, true, false, true))
		{
			FM.Delete(*TempPath, false, true);
			return FM.FileExists(*Path);
		}
		return true;
	}

	bool Load(const FString& Path, FACEBuiltEnvCellMesh& OutMesh, float ExpectedWorldScale, uint64 ExpectedFingerprint)
	{
		if (Path.IsEmpty() || !IFileManager::Get().FileExists(*Path))
		{
			return false;
		}
		TArray<uint8> FileBytes;
		if (!FFileHelper::LoadFileToArray(FileBytes, *Path) || FileBytes.Num() < 40)
		{
			return false;
		}
		FMemoryReader Ar(FileBytes, true);
		uint32 FileMagic = 0, FileSchema = 0, EnvCellId = 0, ScaleBits = 0;
		uint64 Fingerprint = 0;
		uint32 UncompressedSize = 0, CompressedSize = 0;
		if (!ReadPod(Ar, FileMagic) || FileMagic != Magic
			|| !ReadPod(Ar, FileSchema) || FileSchema != SchemaVersion
			|| !ReadPod(Ar, EnvCellId) || !ReadPod(Ar, ScaleBits) || !ReadPod(Ar, Fingerprint)
			|| !ReadPod(Ar, UncompressedSize) || !ReadPod(Ar, CompressedSize))
		{
			return false;
		}
		const uint32 ExpectedScaleBits = *reinterpret_cast<const uint32*>(&ExpectedWorldScale);
		if (ScaleBits != ExpectedScaleBits || Fingerprint != ExpectedFingerprint
			|| CompressedSize == 0 || UncompressedSize == 0 || UncompressedSize > 64u * 1024u * 1024u)
		{
			return false;
		}
		TArray<uint8> Compressed;
		Compressed.SetNumUninitialized(static_cast<int32>(CompressedSize));
		Ar.Serialize(Compressed.GetData(), CompressedSize);
		if (Ar.IsError())
		{
			return false;
		}
		TArray<uint8> Payload;
		Payload.SetNumUninitialized(static_cast<int32>(UncompressedSize));
		if (!FCompression::UncompressMemory(NAME_Zlib, Payload.GetData(), UncompressedSize, Compressed.GetData(), CompressedSize))
		{
			return false;
		}
		FMemoryReader PayloadAr(Payload, true);
		if (!DeserializePayload(PayloadAr, OutMesh))
		{
			return false;
		}
		if (OutMesh.EnvCellId != EnvCellId)
		{
			OutMesh.EnvCellId = EnvCellId;
		}
		ACEDiskTileCache::Touch(Path);
		return true;
	}
}
