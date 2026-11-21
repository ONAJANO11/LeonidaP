using System.Buffers.Binary;
using System.Numerics;
using System.Text.Json;
using RpfPreview.Core;

int checks = 0;
// Synthetic three-joint resource: forward parent reference, reordered palette,
// and nonidentity inverse bind translation. No game data.
byte[] Fixture(bool duplicate = false)
{
    var b = new byte[0x300];
    void P(int at, int target) => BinaryPrimitives.WriteUInt64LittleEndian(b.AsSpan(at), 0x50000000UL + (uint)target);
    void U(int at, ushort value) => BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(at), value);
    P(0x18, 0x40); P(0x58, 0x80);
    P(0xa8, 0x100); P(0xb0, 0x200); P(0xc0, 0x2c0);
    U(0xec, 3); U(0xee, 3);
    ushort[] parents = [ushort.MaxValue, 2, 0];
    for (ushort i = 0; i < 3; i++)
    {
        int bone = 0x100 + i * 0x50;
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(bone + 0x1c), duplicate && i == 2 ? 2U : (uint)i + 1);
        U(bone + 0x46, parents[i]); U(bone + 0x48, i); U(0x2c0 + i * 2, parents[i]);
        foreach (int diagonal in new[] { 0, 5, 10 })
            BinaryPrimitives.WriteSingleLittleEndian(b.AsSpan(0x200 + i * 64 + diagonal * 4), 1);
        BinaryPrimitives.WriteSingleLittleEndian(b.AsSpan(0x200 + i * 64 + 48), -i);
    }
    return b;
}
var mesh = new RageMesh("synthetic", [Vector3.Zero, Vector3.UnitX, Vector3.UnitY], [0, 1, 2])
    { BonePaletteHashes = new uint[] { 3, 2, 3 } };
RageModel Model(RageMesh m) => new("synthetic", [m], Vector3.Zero, Vector3.One) { HasBoneBindings = true };
var exporter = SkeletonExport.Read(Fixture(), 0);
var model = Model(mesh);
string objBefore = RageModelDecoder.CreateObj(model);
using var doc = JsonDocument.Parse(JsonSerializer.Serialize(exporter.CreateMetadata(model)));
var root = doc.RootElement;
string serialized = doc.RootElement.GetRawText();
Check(Enumerable.Range(0, 5).All(_ => JsonSerializer.Serialize(exporter.CreateMetadata(model)) == serialized), "repeated metadata serialization is byte-stable");
var originalCulture = System.Globalization.CultureInfo.CurrentCulture;
try
{
    System.Globalization.CultureInfo.CurrentCulture = System.Globalization.CultureInfo.GetCultureInfo("fr-FR");
    Check(JsonSerializer.Serialize(exporter.CreateMetadata(model)) == serialized, "metadata serialization is culture-independent");
}
finally { System.Globalization.CultureInfo.CurrentCulture = originalCulture; }
Check(!root.GetProperty("HasVertexSkinning").GetBoolean() && !root.GetProperty("HasAnimation").GetBoolean(), "no false skinning/animation claims");
var joints = root.GetProperty("Joints");
Check(joints[0].GetProperty("Parent").ValueKind == JsonValueKind.Null && joints[1].GetProperty("Parent").GetInt32() == 2, "hierarchy survives serialization");
var values = joints[2].GetProperty("InverseBindMatrix").EnumerateArray().Select(x => x.GetSingle()).ToArray();
Check(values.Length == 16 && values[12] == -2 && values[15] == 1, "matrix layout and translation preserved");
// Independent row-vector calculation: (3,0,0,1) times inverse translation -2.
Check(3 * values[0] + values[12] == 1, "reference bind-space x coordinate");
Check(root.GetProperty("Meshes")[0].GetProperty("PaletteJointIndices").EnumerateArray().Select(x => x.GetInt32()).SequenceEqual(new[] { 2, 1, 2 }), "palette slots and repetitions preserved");
Check(RageModelDecoder.CreateObj(model) == objBefore, "metadata export does not mutate OBJ source");
Reject(() => exporter.CreateMetadata(Model(mesh with { BonePaletteHashes = null })), "missing palette");
Reject(() => exporter.CreateMetadata(Model(mesh with { BonePaletteHashes = new uint[] { 999 } })), "unknown joint hash");
Reject(() => SkeletonExport.Read(Fixture(true), 0).CreateMetadata(model), "ambiguous hash");
Reject(() => SkeletonExport.Read(new byte[8], 0), "truncated skeleton");
var nonFinite = Fixture();
BinaryPrimitives.WriteSingleLittleEndian(nonFinite.AsSpan(0x200), float.NaN);
Reject(() => SkeletonExport.Read(nonFinite, 0), "nonfinite inverse bind rejected before output");
var cyclic = Fixture();
BinaryPrimitives.WriteUInt16LittleEndian(cyclic.AsSpan(0x100 + 0x46), 1);
BinaryPrimitives.WriteUInt16LittleEndian(cyclic.AsSpan(0x2c0), 1);
Reject(() => SkeletonExport.Read(cyclic, 0), "cyclic skeleton rejected before output");
Reject(() => exporter.CreateMetadata(new RageModel("empty", [], Vector3.Zero, Vector3.One)), "empty model");
Check(SkeletonExport.FromArguments(["models", "input", "output"]) is null, "default CLI remains opt-out");
try { SkeletonExport.FromArguments(["textures", "a", "b", "--unexpected"]); throw new Exception("unknown CLI accepted"); }
catch (ArgumentException) { checks++; }
foreach (string badOffset in new[] { "-1", "2147483648", "0x10" })
{
    try
    {
        SkeletonExport.FromArguments(["models", "a", "b", "--skeleton-system", "unused", "--skeleton-drawable-offset", badOffset]);
        throw new Exception("invalid offset accepted");
    }
    catch (ArgumentException) { checks++; }
}
string temp = Path.Combine(Path.GetTempPath(), "leonida-synthetic-" + Guid.NewGuid().ToString("N"));
try
{
    File.WriteAllBytes(temp, Fixture());
    var fromFile = SkeletonExport.FromArguments(["models", "a", "b", "--skeleton-system", temp, "--skeleton-drawable-offset", "0"]);
    Check(fromFile is not null && JsonSerializer.Serialize(fromFile.CreateMetadata(model)) == doc.RootElement.GetRawText(), "explicit file and offset preserve JSON output");
    Reject(() => SkeletonExport.FromArguments(["models", "a", "b", "--skeleton-system", temp, "--skeleton-drawable-offset", "768"]), "drawable offset outside page");
    var mixed = new RageModel("mixed", [mesh, mesh with { BonePaletteHashes = null }], Vector3.Zero, Vector3.One);
    Reject(() => exporter.CreateMetadata(mixed), "partially resolvable model rejected as a whole");
}
finally { if (File.Exists(temp)) File.Delete(temp); }
string outputDir = Path.Combine(Path.GetTempPath(), "leonida-sidecar-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(outputDir);
try
{
    string sidecar = Path.Combine(outputDir, "synthetic.skeleton.json");
    var options = new JsonSerializerOptions { WriteIndented = true };
    string expected = JsonSerializer.Serialize(exporter.CreateMetadata(model), options);
    File.WriteAllText(sidecar + ".partial", "stale interrupted output");
    SkeletonExport.WriteMetadata(sidecar, exporter.CreateMetadata(model), options);
    Check(File.ReadAllText(sidecar) == expected && !File.Exists(sidecar + ".partial"), "atomic sidecar replaces stale temporary output");
    File.WriteAllText(sidecar, "previous version");
    SkeletonExport.WriteMetadata(sidecar, exporter.CreateMetadata(model), options);
    Check(File.ReadAllText(sidecar) == expected, "atomic sidecar replaces an existing final file");
    try
    {
        Util.WriteAtomic(sidecar, temporary => { File.WriteAllText(temporary, "truncated"); throw new IOException("synthetic write failure"); });
        throw new Exception("atomic writer swallowed failure");
    }
    catch (IOException) { checks++; }
    Check(File.ReadAllText(sidecar) == expected && !File.Exists(sidecar + ".partial"), "interrupted replacement preserves previous valid sidecar");
    try
    {
        SkeletonExport.WriteMetadata(sidecar, new { NonFinite = double.NaN }, options);
        throw new Exception("invalid JSON number was serialized");
    }
    catch (ArgumentException) { checks++; }
    Check(File.ReadAllText(sidecar) == expected && !File.Exists(sidecar + ".partial"), "serialization failure preserves previous valid sidecar");
    string blocked = Path.Combine(outputDir, "blocked.skeleton.json");
    Directory.CreateDirectory(blocked);
    try
    {
        SkeletonExport.WriteMetadata(blocked, exporter.CreateMetadata(model), options);
        throw new Exception("failed destination was accepted");
    }
    catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { checks++; }
    Check(!File.Exists(blocked + ".partial"), "failed rename cleans up sidecar temporary file");
    Directory.Delete(blocked);
    Check(RageModelDecoder.CreateObj(model) == objBefore, "atomic sidecar path preserves OBJ serialization");
}
finally
{
    // Delete only the named synthetic outputs; never recurse through a caller path.
    foreach (string file in Directory.GetFiles(outputDir)) File.Delete(file);
    string blocked = Path.Combine(outputDir, "blocked.skeleton.json");
    if (Directory.Exists(blocked)) Directory.Delete(blocked);
    Directory.Delete(outputDir);
}
Console.WriteLine($"Skeleton metadata export: {checks} checks passed (synthetic, CPU only).");
void Check(bool ok, string why) { if (!ok) throw new Exception(why); checks++; }
void Reject(Action action, string why)
{
    try { action(); } catch (InvalidDataException) { checks++; return; }
    throw new Exception("Expected rejection: " + why);
}
