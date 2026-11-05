<#
.SYNOPSIS
Wraps a native image PE (the zapper's output) into the XEX2 DLL module xcoreclr loads as <name>.ni.dll.

.DESCRIPTION
The layout follows the Plex runtime's .ni.dll files (PlexResources.ni.dll, for one): module flags title|DLL, no
imports or exports, basic compression, 4 KB pages for images at or above 0x90000000, image flags
PAGE_SIZE_4KB|REVOCATION_CHECK_REQUIRED, the execution info of the app's title, and the image ending before the PE's
trailing .reloc section. Nothing is signed; xenia doesn't check the RSA signature (a retail console would reject the
module).

With -Base it first rebases the image to that load address, as Microsoft's pipeline must have: xcoreclr's zapper
writes images based at 0 (docs\RESEARCH.md). The byte work is a C# class (Add-Type), for the size of the images:
runs in Windows PowerShell 5.1 and PowerShell 7 alike.

.EXAMPLE
.\tools\pe2nixex.ps1 stage\mscorlib.ni.dll package\mscorlib.ni.dll -Template package\default.xex -Base 95000000
#>
param(
    [Parameter(Mandatory, Position = 0)] [string]$Pe,
    [Parameter(Mandatory, Position = 1)] [string]$Xex,
    # An XEX of the app (default.xex or a .ni.dll) to take the title from.
    [string]$Template = '',
    # The title ID (hex), instead of the template's.
    [string]$TitleId = '',
    # The original PE name (default: the output file's name).
    [string]$Name = '',
    # Rebases the image to this load address (hex), a multiple of 64 KB.
    [string]$Base = ''
)
$ErrorActionPreference = 'Stop'

if (-not ('XboxNGen.NiXex' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace XboxNGen {
public static class NiXex {
    // xenia:src/xenia/kernel/util/xex2_info.h
    const uint HeaderFileFormatInfo = 0x000003FF, HeaderImageBaseAddress = 0x00010201, HeaderChecksumTimestamp = 0x00018002,
        HeaderOriginalPeName = 0x000183FF, HeaderSystemFlags = 0x00030000, HeaderSystemFlags32 = 0x00030100,
        HeaderExecutionInfo = 0x00040006, HeaderLanKey = 0x00040404;
    const uint ModuleTitle = 1, ModuleDll = 8, ImagePageSize4KB = 0x10000000, ImageRevocationCheckRequired = 0x80000000;
    const int SectionCode = 1, SectionData = 2, SectionReadonly = 3;
    const int SecurityInfoBytes = 0x184, ImageInfoBytes = 0x174;
    const ushort CompressionBasic = 1;
    // The values the Plex .ni.dll files carry for these headers.
    const uint SystemFlags = 0x00000524, SystemFlags32 = 0x00003210;

    class Section { public string Name; public int Va; public int VSize; public byte[] Raw; public uint Flags; }

    static uint LE32(byte[] b, int at) { return BitConverter.ToUInt32(b, at); }
    static ushort LE16(byte[] b, int at) { return BitConverter.ToUInt16(b, at); }
    static uint BE32(byte[] b, int at) { return (uint)(b[at] << 24 | b[at + 1] << 16 | b[at + 2] << 8 | b[at + 3]); }
    static ushort BE16(byte[] b, int at) { return (ushort)(b[at] << 8 | b[at + 1]); }
    static void PutLE32(byte[] b, int at, uint v) { b[at] = (byte)v; b[at + 1] = (byte)(v >> 8); b[at + 2] = (byte)(v >> 16); b[at + 3] = (byte)(v >> 24); }
    static void PutBE32(byte[] b, int at, uint v) { b[at] = (byte)(v >> 24); b[at + 1] = (byte)(v >> 16); b[at + 2] = (byte)(v >> 8); b[at + 3] = (byte)v; }
    static void PutBE16(byte[] b, int at, int v) { b[at] = (byte)(v >> 8); b[at + 1] = (byte)v; }
    static byte[] BE(params uint[] values) { var b = new byte[4 * values.Length]; for (int i = 0; i < values.Length; i++) PutBE32(b, 4 * i, values[i]); return b; }
    static int Align(int value, int alignment) { return (value + alignment - 1) & ~(alignment - 1); }
    static long FloorDiv(long a, long b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

    static byte[] Sha1(params byte[][] parts) {
        using (var sha = SHA1.Create()) {
            foreach (var part in parts) sha.TransformBlock(part, 0, part.Length, null, 0);
            sha.TransformFinalBlock(new byte[0], 0, 0);
            return sha.Hash;
        }
    }

    static byte[] Slice(byte[] b, int start, int end) {
        if (end > b.Length) end = b.Length;
        if (start > end) start = end;
        var r = new byte[end - start];
        Buffer.BlockCopy(b, start, r, 0, r.Length);
        return r;
    }

    // Writes source at offset, growing the image the way a Python slice assignment past its end does.
    static byte[] Place(byte[] image, int offset, byte[] source) {
        if (offset + source.Length > image.Length) Array.Resize(ref image, offset + source.Length);
        Buffer.BlockCopy(source, 0, image, offset, source.Length);
        return image;
    }

    // The image with its sections at their RVAs, the PE's base, its sections, timestamp and checksum.
    static byte[] LoadPe(byte[] data, out uint imageBase, out List<Section> sections, out uint timestamp, out uint checksum) {
        int e = (int)LE32(data, 0x3C);
        if (data[e] != 'P' || data[e + 1] != 'E' || data[e + 2] != 0 || data[e + 3] != 0) throw new Exception("not a PE file");
        ushort machine = LE16(data, e + 4), count = LE16(data, e + 6);
        timestamp = LE32(data, e + 8);
        ushort optionalSize = LE16(data, e + 20);
        if (machine != 0x1F2) throw new Exception(string.Format("machine 0x{0:x} is not PowerPC big-endian", machine));
        int opt = e + 24;
        imageBase = LE32(data, opt + 28);
        int imageSize = (int)LE32(data, opt + 56), headersSize = (int)LE32(data, opt + 60);
        checksum = LE32(data, opt + 64);
        sections = new List<Section>();
        for (int i = 0; i < count; i++) {
            int at = opt + optionalSize + 40 * i;
            string name = Encoding.ASCII.GetString(data, at, 8).TrimEnd('\0');
            int vsize = (int)LE32(data, at + 8), va = (int)LE32(data, at + 12), rsize = (int)LE32(data, at + 16), rptr = (int)LE32(data, at + 20);
            int length = Math.Min(rsize, vsize != 0 ? vsize : rsize);
            sections.Add(new Section { Name = name, Va = va, VSize = vsize != 0 ? vsize : rsize, Raw = Slice(data, rptr, rptr + length), Flags = LE32(data, at + 36) });
        }
        var image = new byte[imageSize];
        image = Place(image, 0, Slice(data, 0, headersSize));
        foreach (var s in sections) image = Place(image, s.Va, s.Raw);
        return image;
    }

    // Applies the PE's base relocations for a move from oldBase to newBase, moving only words whose value lies inside
    // the image: the zapper also lists precodes' `lis/addi r12` of xcoreclr's fixup thunks, which stay put
    // (docs\RESEARCH.md). Then the addresses the zapper leaves as RVAs with no relocations: the exception directory's
    // RUNTIME_FUNCTIONs, CORCOMPILE_HEADER.ImageBase (offset 0x88) and the reverse P/Invoke thunk records' MethodDesc
    // and two pointers (+4, +0x74, +0x78 of each 0x7C-byte record).
    static void Rebase(byte[] image, uint oldBase, uint newBase) {
        uint delta = newBase - oldBase;
        int e = (int)LE32(image, 0x3C);
        long sizeOfImage = LE32(image, e + 24 + 56);
        Func<long, bool> inside = v => oldBase <= v && v < oldBase + sizeOfImage;
        int relocRva = (int)LE32(image, e + 24 + 96 + 5 * 8), relocSize = (int)LE32(image, e + 24 + 96 + 5 * 8 + 4);
        int offset = relocRva, end = relocRva + relocSize;
        // The HIGHADJ decisions not yet claimed by a LOW, in order: two lis may precede their two loads.
        var highInside = new Queue<bool>();
        while (offset < end) {
            int page = (int)LE32(image, offset), block = (int)LE32(image, offset + 4);
            if (block == 0) break;
            int entries = (block - 8) / 2;
            for (int i = 0; i < entries; i++) {
                int entry = LE16(image, offset + 8 + 2 * i);
                int kind = entry >> 12, at = page + (entry & 0xFFF);
                if (kind == 3) {
                    uint value = BE32(image, at);
                    if (inside(value)) PutBE32(image, at, value + delta);
                } else if (kind == 4) {
                    // The zapper writes the low half's entry big-endian, like the instruction it belongs to.
                    int low = BE16(image, offset + 8 + 2 * (i + 1));
                    int high = BE16(image, at + 2);
                    long value = (((long)high << 16) + ((low & 0x8000) != 0 ? low - 0x10000 : low)) & 0xFFFFFFFF;
                    bool moves = inside(value);
                    highInside.Enqueue(moves);
                    if (moves) {
                        value = (value + delta) & 0xFFFFFFFF;
                        PutBE16(image, at + 2, (int)(((value + 0x8000) >> 16) & 0xFFFF));
                    }
                    i++;
                } else if (kind == 2) {
                    // The LOWs follow their HIGHADJs in the table in the same order: a pair moves or stays together.
                    if (highInside.Count == 0) throw new Exception(string.Format("LOW relocation at 0x{0:x} without a HIGHADJ before it", at));
                    if (highInside.Dequeue()) PutBE16(image, at + 2, (int)((BE16(image, at + 2) + delta) & 0xFFFF));
                } else if (kind != 0) {
                    throw new Exception(string.Format("relocation kind {0} at 0x{1:x} is not handled", kind, at));
                }
            }
            offset += block;
        }
        PutLE32(image, e + 24 + 28, newBase);
        int pdataRva = (int)LE32(image, e + 24 + 96 + 3 * 8), pdataSize = (int)LE32(image, e + 24 + 96 + 3 * 8 + 4);
        for (int at = pdataRva; at < pdataRva + pdataSize; at += 8) {
            uint begin = BE32(image, at);
            if (begin != 0 && begin < image.Length) PutBE32(image, at, begin + newBase);
        }
        int cor20 = (int)LE32(image, e + 24 + 96 + 14 * 8);
        int nativeHeader = (int)LE32(image, cor20 + 0x40);
        if (nativeHeader != 0) {
            PutBE32(image, nativeHeader + 0x88, newBase);
            int records = (int)LE32(image, nativeHeader + 0xA4), size = (int)LE32(image, nativeHeader + 0xA8);
            for (int at = records; at < records + size; at += 0x7C) {
                foreach (int field in new[] { 4, 0x74, 0x78 }) {
                    uint value = BE32(image, at + field);
                    if (value != 0 && value < image.Length) PutBE32(image, at + field, value + newBase);
                }
            }
        }
    }

    static int SectionKind(uint flags) {
        if ((flags & 0x20000000) != 0) return SectionCode;
        if ((flags & 0x80000000) != 0) return SectionData;
        return SectionReadonly;
    }

    // Runs of pages of one kind. A descriptor covers at most 64 KB and never crosses a 64 KB boundary: the console's
    // hypervisor checks both (HvxLoadImageData) and the loader fails the image otherwise. xenia checks neither.
    static List<int[]> PageRuns(int imageBytes, List<Section> sections, int page) {
        var kinds = new int[imageBytes / page];
        kinds[0] = SectionReadonly;
        foreach (var s in sections) {
            long first = FloorDiv(s.Va, page), last = FloorDiv((long)s.Va + s.VSize - 1, page);
            for (long p = first; p <= Math.Min(last, kinds.Length - 1); p++) {
                int k = SectionKind(s.Flags);
                // A page holding code and data is code, as the zapper keeps them on separate pages anyway.
                if (kinds[p] == 0 || kinds[p] == SectionReadonly || k == SectionCode) kinds[p] = k;
            }
        }
        var runs = new List<int[]>();
        for (int index = 0; index < kinds.Length; index++) {
            int kind = kinds[index] != 0 ? kinds[index] : SectionReadonly;
            if (runs.Count > 0 && runs[runs.Count - 1][0] == kind && ((long)index * page) % 0x10000 != 0) runs[runs.Count - 1][1]++;
            else runs.Add(new[] { kind, 1 });
        }
        return runs;
    }

    // The page descriptors, each with the digest of the next run, and the image digest (of the first run).
    static byte[] PageDescriptors(byte[] image, List<int[]> runs, int page, out byte[] imageDigest) {
        var nextDigest = new byte[20];
        var descriptors = new List<byte[]>();
        int offset = image.Length;
        for (int r = runs.Count - 1; r >= 0; r--) {
            int kind = runs[r][0], count = runs[r][1];
            offset -= count * page;
            var descriptor = new byte[24];
            PutBE32(descriptor, 0, (uint)(count << 4 | kind));
            Buffer.BlockCopy(nextDigest, 0, descriptor, 4, 20);
            descriptors.Insert(0, descriptor);
            nextDigest = Sha1(Slice(image, offset, offset + count * page), descriptor);
        }
        imageDigest = nextDigest;
        var all = new byte[24 * descriptors.Count];
        for (int i = 0; i < descriptors.Count; i++) Buffer.BlockCopy(descriptors[i], 0, all, 24 * i, 24);
        return all;
    }

    // Basic compression: runs of 4 KB pages stored, each followed by the zero pages after it.
    static List<int[]> CompressionBlocks(byte[] image) {
        var blocks = new List<int[]>();
        for (int offset = 0; offset < image.Length; offset += 0x1000) {
            bool zero = true;
            for (int i = offset; i < offset + 0x1000 && i < image.Length; i++) if (image[i] != 0) { zero = false; break; }
            if (zero && blocks.Count > 0) blocks[blocks.Count - 1][1] += 0x1000;
            else if (blocks.Count > 0 && blocks[blocks.Count - 1][1] == 0) blocks[blocks.Count - 1][0] += 0x1000;
            else blocks.Add(new[] { 0x1000, 0 });
        }
        blocks[blocks.Count - 1][1] = 0;
        return blocks;
    }

    public static byte[] Build(byte[] peBytes, string peName, uint titleId, uint mediaId, uint version, uint baseVersion, long newBase) {
        uint imageBase, timestamp, checksum;
        List<Section> sections;
        byte[] image = LoadPe(peBytes, out imageBase, out sections, out timestamp, out checksum);
        if (newBase >= 0 && newBase != imageBase) {
            if ((newBase & 0xFFFF) != 0) throw new Exception("the base must be a multiple of 64 KB");
            Rebase(image, imageBase, (uint)newBase);
            imageBase = (uint)newBase;
        }
        // The XEX image stops where a trailing .reloc section starts, as in the platform's .ni.dll files.
        if (sections.Count > 0 && sections[sections.Count - 1].Name == ".reloc") {
            image = Slice(image, 0, sections[sections.Count - 1].Va);
            sections.RemoveAt(sections.Count - 1);
        }
        int page = imageBase >= 0x90000000 ? 0x1000 : 0x10000;
        Array.Resize(ref image, Align(image.Length, page));
        var runs = PageRuns(image.Length, sections, page);
        byte[] imageDigest;
        byte[] descriptors = PageDescriptors(image, runs, page, out imageDigest);
        var blocks = CompressionBlocks(image);
        var fileData = new MemoryStream();
        int offset = 0;
        foreach (var b in blocks) { fileData.Write(image, offset, b[0]); offset += b[0] + b[1]; }

        byte[] name = Encoding.UTF8.GetBytes(peName + "\0");
        var nameBlock = new byte[4 + Align(name.Length, 4)];
        PutBE32(nameBlock, 0, (uint)nameBlock.Length);
        Buffer.BlockCopy(name, 0, nameBlock, 4, name.Length);
        var formatInfo = new MemoryStream();
        formatInfo.Write(BE((uint)(8 + 8 * blocks.Count), CompressionBasic), 0, 8);
        foreach (var b in blocks) formatInfo.Write(BE((uint)b[0], (uint)b[1]), 0, 8);
        var executionInfo = new byte[24];
        Buffer.BlockCopy(BE(mediaId, version, baseVersion, titleId), 0, executionInfo, 0, 16);
        var inline = new SortedDictionary<uint, uint> {
            { HeaderImageBaseAddress, imageBase }, { HeaderSystemFlags, SystemFlags }, { HeaderSystemFlags32, SystemFlags32 } };
        var dataBlocks = new SortedDictionary<uint, byte[]> {
            { HeaderFileFormatInfo, formatInfo.ToArray() }, { HeaderChecksumTimestamp, BE(checksum, timestamp) },
            { HeaderOriginalPeName, nameBlock }, { HeaderExecutionInfo, executionInfo }, { HeaderLanKey, new byte[16] } };
        int count = inline.Count + dataBlocks.Count;
        int securityOffset = Align(0x18 + 8 * count, 8);
        int securityBytes = SecurityInfoBytes + descriptors.Length;
        int dataOffset = Align(securityOffset + securityBytes, 8);
        var keys = new List<uint>(inline.Keys);
        keys.AddRange(dataBlocks.Keys);
        keys.Sort();
        var table = new MemoryStream();
        var data = new MemoryStream();
        foreach (uint key in keys) {
            if (inline.ContainsKey(key)) { table.Write(BE(key, inline[key]), 0, 8); continue; }
            table.Write(BE(key, (uint)(dataOffset + data.Length)), 0, 8);
            byte[] block = dataBlocks[key];
            data.Write(block, 0, block.Length);
            data.Write(new byte[Align(block.Length, 8) - block.Length], 0, Align(block.Length, 8) - block.Length);
        }
        int headerSize = Align(dataOffset + (int)data.Length, 0x1000);

        var security = new byte[SecurityInfoBytes];
        PutBE32(security, 0, (uint)securityBytes);
        PutBE32(security, 4, (uint)image.Length);
        uint flags = ImageRevocationCheckRequired | (page == 0x1000 ? ImagePageSize4KB : 0);
        PutBE32(security, 0x108, ImageInfoBytes);
        PutBE32(security, 0x10C, flags);
        PutBE32(security, 0x110, imageBase);
        Buffer.BlockCopy(imageDigest, 0, security, 0x114, 20);
        PutBE32(security, 0x178, 0xFFFFFFFF);
        PutBE32(security, 0x17C, 0xFFFFFFFF);
        PutBE32(security, 0x180, (uint)runs.Count);

        var header = new byte[headerSize];
        header[0] = (byte)'X'; header[1] = (byte)'E'; header[2] = (byte)'X'; header[3] = (byte)'2';
        PutBE32(header, 4, ModuleTitle | ModuleDll);
        PutBE32(header, 8, (uint)headerSize);
        PutBE32(header, 16, (uint)securityOffset);
        PutBE32(header, 20, (uint)count);
        table.ToArray().CopyTo(header, 0x18);
        security.CopyTo(header, securityOffset);
        descriptors.CopyTo(header, securityOffset + SecurityInfoBytes);
        data.ToArray().CopyTo(header, dataOffset);
        byte[] digest = Sha1(Slice(header, securityOffset + 0x17C, header.Length), Slice(header, 0, securityOffset + 8));
        Buffer.BlockCopy(digest, 0, header, securityOffset + 0x164, 20);
        var result = new byte[header.Length + fileData.Length];
        header.CopyTo(result, 0);
        fileData.ToArray().CopyTo(result, header.Length);
        return result;
    }

    // The app's media ID, version, base version and title ID from its XEX; zeros when it declares no title, as a
    // Silverlight360 runtime profile's default.xex doesn't.
    public static uint[] ReadExecutionInfo(byte[] xex) {
        uint count = BE32(xex, 0x14);
        for (int i = 0; i < count; i++) {
            if (BE32(xex, 0x18 + 8 * i) != HeaderExecutionInfo) continue;
            int value = (int)BE32(xex, 0x18 + 8 * i + 4);
            return new[] { BE32(xex, value), BE32(xex, value + 4), BE32(xex, value + 8), BE32(xex, value + 12) };
        }
        return new uint[4];
    }
}
}
'@
}

$info = if ($Template) { [XboxNGen.NiXex]::ReadExecutionInfo([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Template).Path)) } else { [uint32[]]@(0, 0, 0, 0) }
$title = if ($TitleId) { [Convert]::ToUInt32($TitleId, 16) } else { $info[3] }
$moduleName = if ($Name) { $Name } else { Split-Path -Leaf $Xex }
$newBase = if ($Base) { [long][Convert]::ToUInt32($Base, 16) } else { [long]-1 }
$bytes = [XboxNGen.NiXex]::Build([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Pe).Path), $moduleName, $title, $info[0], $info[1], $info[2], $newBase)
[IO.File]::WriteAllBytes([IO.Path]::GetFullPath($Xex), $bytes)
