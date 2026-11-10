using System;
using System.Collections.Generic;
using System.IO.MemoryMappedFiles;
using System.Text;
using System.Threading;

public static class HarukotoRuntimeStatusReader {
    // A missing mapping is intentionally distinct from an empty live mapping.
    public static string[] Read(int processId, long creationFileTime) {
        string name = "Local\\Harukoto.Runtime.v1." + processId + "." + creationFileTime;
        using (var map = MemoryMappedFile.OpenExisting(name, MemoryMappedFileRights.Read))
        using (var view = map.CreateViewAccessor(0, 65536, MemoryMappedFileAccess.Read)) {
            var results = new List<string>();
            for (int index = 0; index < 64; ++index) {
                long offset = index * 1024;
                for (int attempt = 0; attempt < 3; ++attempt) {
                    int owner = view.ReadInt32(offset);
                    if (owner == 0) break;
                    int sequence = view.ReadInt32(offset + 4);
                    if (owner < 0 || (sequence & 1) != 0) continue;
                    var bytes = new byte[1016];
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
}
