using System;
using System.Collections.Generic;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Text;
using System.Threading;

public static class HarukotoRuntimeStatusReader {
    private static string[] ReadVersion(string prefix, int slotBytes, int processId, long creationFileTime) {
        string name = prefix + processId + "." + creationFileTime;
        using (var map = MemoryMappedFile.OpenExisting(name, MemoryMappedFileRights.Read))
        using (var view = map.CreateViewAccessor(0, 64L * slotBytes, MemoryMappedFileAccess.Read)) {
            var results = new List<string>();
            int jsonBytes = slotBytes - 8;
            for (int index = 0; index < 64; ++index) {
                long offset = (long)index * slotBytes;
                for (int attempt = 0; attempt < 3; ++attempt) {
                    int owner = view.ReadInt32(offset);
                    if (owner == 0) break;
                    int sequence = view.ReadInt32(offset + 4);
                    if (owner < 0 || (sequence & 1) != 0) continue;
                    var bytes = new byte[jsonBytes];
                    view.ReadArray(offset + 8, bytes, 0, bytes.Length);
                    Thread.MemoryBarrier();
                    if (sequence != view.ReadInt32(offset + 4) || owner != view.ReadInt32(offset)) continue;
                    int end = Array.IndexOf(bytes, (byte)0);
                    if (end <= 0) break;
                    results.Add(new UTF8Encoding(false, true).GetString(bytes, 0, end));
                    break;
                }
            }
            return results.ToArray();
        }
    }

    // Read v2 (2048-byte slots, RH-4+) first, falling back to v1 (1024-byte
    // slots) so Diagnose still inspects already-open older DLL processes.
    // A missing mapping is intentionally distinct from an empty live mapping.
    public static string[] Read(int processId, long creationFileTime) {
        try {
            return ReadVersion("Local\\Harukoto.Runtime.v2.", 2048, processId, creationFileTime);
        } catch (FileNotFoundException) {
            return ReadVersion("Local\\Harukoto.Runtime.v1.", 1024, processId, creationFileTime);
        }
    }
}
