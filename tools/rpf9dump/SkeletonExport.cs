using System.Globalization;
using System.Numerics;
using System.Text.Json;
using RpfPreview.Core;

// Metadata only: palette slots are not per-vertex joint indices or weights.
internal sealed class SkeletonExport
{
    private readonly RageSkeleton skeleton;
    private SkeletonExport(RageSkeleton skeleton) => this.skeleton = skeleton;

    public static SkeletonExport? FromArguments(string[] args)
    {
        if (args.Length == 3) return null;
        if (args.Length != 7 || args[0] != "models" || args[3] != "--skeleton-system" ||
            args[5] != "--skeleton-drawable-offset")
            throw new ArgumentException("Optional models arguments: --skeleton-system <system-page-file> --skeleton-drawable-offset <decimal-offset>");
        if (!int.TryParse(args[6], NumberStyles.None, CultureInfo.InvariantCulture, out int offset))
            throw new ArgumentException("Skeleton drawable offset must be a nonnegative decimal integer.");
        return Read(File.ReadAllBytes(args[4]), offset);
    }

    internal static SkeletonExport Read(ReadOnlySpan<byte> system, int drawableOffset) =>
        new(RageSkeleton.ReadForDrawable(system, drawableOffset) ??
            throw new InvalidDataException("The selected system page does not contain a valid skeleton at that drawable offset."));

    internal object CreateMetadata(RageModel model)
    {
        var meshes = new List<object>();
        foreach (var mesh in model.Meshes)
        {
            var hashes = mesh.BonePaletteHashes;
            if (hashes is null || hashes.Count == 0)
                throw new InvalidDataException("A selected mesh has no verified bone palette; skeleton metadata was not exported.");
            if (!skeleton.TryResolvePalette(hashes, out var joints))
                throw new InvalidDataException("A mesh palette has missing or ambiguous skeleton hashes; skeleton metadata was not exported.");
            meshes.Add(new { mesh.Name, PaletteHashes = hashes.ToArray(), PaletteJointIndices = joints });
        }
        if (meshes.Count == 0) throw new InvalidDataException("The model contains no meshes.");
        return new
        {
            SchemaVersion = 1,
            Kind = "skeleton-metadata",
            CoordinateSystem = "source-unchanged",
            MatrixConvention = "System.Numerics row-vector, row-major elements; translation at indices 12..14",
            HasVertexSkinning = false,
            HasAnimation = false,
            Joints = skeleton.Joints.Select(j => new { j.Hash, j.Parent, InverseBindMatrix = Elements(j.InverseBindMatrix) }).ToArray(),
            Meshes = meshes
        };
    }

    internal static void WriteMetadata(string path, object metadata, JsonSerializerOptions options) =>
        Util.WriteAllText(path, JsonSerializer.Serialize(metadata, options));

    private static float[] Elements(Matrix4x4 m) =>
        [m.M11, m.M12, m.M13, m.M14, m.M21, m.M22, m.M23, m.M24,
         m.M31, m.M32, m.M33, m.M34, m.M41, m.M42, m.M43, m.M44];
}
