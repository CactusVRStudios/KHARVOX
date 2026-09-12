using System.IO.Compression;

namespace KharvoxLauncher;

// Native managed Paula playback. The build-time SIDMON II decoder records a whole
// song loop as register writes and sample-memory edits, not prerecorded PCM audio.
internal sealed class AmigaAudioEngine
{
    private sealed class Tick
    {
        internal (int Offset, byte Value)[] Patches = Array.Empty<(int, byte)>();
        internal (byte Id, int Value)[] Writes = Array.Empty<(byte, int)>();
        internal readonly int[] Volumes = new int[4];
    }
    private sealed class Channel
    {
        internal int Enabled, Pointer, Length, Period = 50, Volume, Position, End, Delay;
        internal double Timer, Held;
        internal void Write(int register, int value)
        {
            switch (register)
            {
                case 0:
                    if (Enabled == value) break;
                    Enabled = value; Position = Pointer; End = Pointer + Length; Timer = 1;
                    if (value != 0) Delay += 2;
                    break;
                case 1: Pointer = value; break;
                case 2: Length = value; break;
                case 3: Period = Math.Max(0, Math.Min(65535, value)); break;
                case 4: Volume = Math.Max(0, Math.Min(64, value)); break;
            }
        }
        internal double Sample(byte[] memory)
        {
            if (Enabled != 0 && Period > 60)
            {
                if (Delay > 0) Delay--;
                else if (--Timer < 1)
                {
                    Held = (Position >= 0 && Position < memory.Length ? unchecked((sbyte)memory[Position]) : 0) * Volume / 32768.0;
                    Position++;
                    Timer += Period * 44100.0 / 3546895.0;
                    if (Position >= End) { Position = Pointer; End = Pointer + Length; }
                }
            }
            return Held;
        }
    }
    private readonly byte[] initial, memory;
    private readonly Tick[] ticks;
    private readonly Channel[] channels = { new(), new(), new(), new() };
    private int tickIndex, samplesLeft;

    internal AmigaAudioEngine()
    {
        using var stream = CracktroForm.Resource("Amiga.possessed.paula.gz");
        using var gzip = new GZipStream(stream, CompressionMode.Decompress);
        using var reader = new BinaryReader(gzip);
        if (new string(reader.ReadChars(4)) != "KPA1") throw new InvalidDataException("Invalid Paula sequence.");
        int size = reader.ReadInt32(), count = reader.ReadInt32();
        if (size < 1 || size > 1048576 || count < 1 || count > 18000) throw new InvalidDataException("Invalid Paula sequence dimensions.");
        initial = reader.ReadBytes(size);
        if (initial.Length != size) throw new EndOfStreamException();
        memory = (byte[])initial.Clone();
        ticks = new Tick[count];
        var volumes = new int[4];
        for (int i = 0; i < count; i++)
        {
            var tick = ticks[i] = new Tick();
            tick.Patches = new (int, byte)[reader.ReadUInt16()];
            for (int p = 0; p < tick.Patches.Length; p++)
            {
                int offset = reader.ReadInt32(); byte value = reader.ReadByte();
                if (offset < 0 || offset >= size) throw new InvalidDataException("Invalid Paula sample offset.");
                tick.Patches[p] = (offset, value);
            }
            tick.Writes = new (byte, int)[reader.ReadUInt16()];
            for (int w = 0; w < tick.Writes.Length; w++)
            {
                byte id = reader.ReadByte(); int value = reader.ReadInt32();
                if (id >= 20) throw new InvalidDataException("Invalid Paula register.");
                tick.Writes[w] = (id, value);
                if (id % 5 == 4) volumes[id / 5] = value;
            }
            Array.Copy(volumes, tick.Volumes, 4);
        }
    }

    internal int VolumeAt(double seconds, int channel) => ticks[(int)(Math.Max(0, seconds) * 50) % ticks.Length].Volumes[channel];

    internal void Fill(short[] output)
    {
        for (int i = 0; i < output.Length; i++)
        {
            if (samplesLeft == 0)
            {
                if (tickIndex == ticks.Length)
                {
                    tickIndex = 0; Array.Copy(initial, memory, initial.Length);
                    for (int c = 0; c < channels.Length; c++) channels[c] = new Channel();
                }
                var tick = ticks[tickIndex++];
                foreach (var patch in tick.Patches) memory[patch.Offset] = patch.Value;
                foreach (var write in tick.Writes) channels[write.Id / 5].Write(write.Id % 5, write.Value);
                samplesLeft = 882; // PAL 50 Hz at 44100 Hz output
            }
            double sample = 0;
            foreach (var channel in channels) sample += channel.Sample(memory);
            output[i] = (short)Math.Max(-32768, Math.Min(32767, sample * 30000));
            samplesLeft--;
        }
    }
}
