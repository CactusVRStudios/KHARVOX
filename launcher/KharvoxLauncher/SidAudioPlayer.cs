using System.Runtime.InteropServices;
using EMU6502;

// Small replacements for the two Unity float math helpers used by the MIT SID core.
internal static class SidMath
{
    internal static float Sin(float x) => (float)Math.Sin(x);
    internal static float Pow(float x, float y) => (float)Math.Pow(x, y);
}

namespace KharvoxLauncher
{
    // PSID host for the embedded single-SID tune; no C64 ROM or filesystem is needed.
    internal sealed class IntroSidEngine
    {
        private readonly RAM64K ram = new();
        private readonly MOS6502 cpu;
        private readonly SID sid;
        private readonly ushort playAddress;
        private readonly bool ciaTiming;
        private const ushort Trampoline = 0x0200, Vector = 0x02f0;

        internal IntroSidEngine()
        {
            using var stream = CracktroForm.Resource("Coco_Intro.sid");
            using var reader = new BinaryReader(stream);
            var data = reader.ReadBytes((int)stream.Length);
            int Big16(int p) => data[p] << 8 | data[p + 1];
            if (data.Length < 126 || System.Text.Encoding.ASCII.GetString(data, 0, 4) != "PSID")
                throw new InvalidDataException("Invalid embedded PSID file.");
            int offset = Big16(6), load = Big16(8);
            if (load == 0) { load = data[offset] | data[offset + 1] << 8; offset += 2; }
            if (load < 0x300 || load + data.Length - offset > 65536)
                throw new InvalidDataException("Unsupported SID load range.");
            ram.Load(data.Skip(offset).ToArray(), (ushort)load);
            ram.WriteRAM(1, 0x37);
            ram.WriteIO(0xdc04, 0x25); ram.WriteIO(0xdc05, 0x40);
            ram.Write16(Vector, Trampoline);
            cpu = new MOS6502(ram);
            cpu.Process(); // complete CPU reset before calling the tune
            sid = new SID(ram);
            cpu.A = (byte)(Big16(16) - 1); cpu.X = 0; cpu.Y = 0;
            Call((ushort)Big16(10), false);
            playAddress = (ushort)Big16(12);
            if (playAddress == 0) playAddress = ram.Read16(0x0314);
            if (playAddress == 0) throw new InvalidDataException("SID has no play routine.");
            ciaTiming = (data[21] & 1) != 0;
        }

        private int Call(ushort address, bool audio)
        {
            ram.WriteRAM(Trampoline, 0x20); // JSR tune; stop on returning to the trampoline
            ram.WriteRAM(Trampoline + 1, (byte)address);
            ram.WriteRAM(Trampoline + 2, (byte)(address >> 8));
            cpu.Jump(Vector);
            cpu.Cycles = 0;
            int instructions = 0;
            do
            {
                int before = cpu.Cycles;
                cpu.Process();
                if (cpu.Jam || ++instructions > 100000)
                    throw new InvalidDataException("SID routine did not return.");
                if (audio) sid.BufferSamples(cpu.Cycles - before);
            } while (cpu.PC != Trampoline + 3);
            return cpu.Cycles;
        }

        internal void Fill(short[] output)
        {
            while (sid.samples.Count < output.Length)
            {
                int cycles = Call(playAddress, true);
                int frameCycles = ciaTiming ? ram.ReadIO(0xdc04) | ram.ReadIO(0xdc05) << 8 : 19656;
                if (frameCycles < 1000) throw new InvalidDataException("Unsupported SID timer rate.");
                sid.BufferSamples(Math.Max(0, frameCycles - cycles));
            }
            for (int i = 0; i < output.Length; i++)
                output[i] = (short)Math.Max(short.MinValue, Math.Min(short.MaxValue, sid.samples[i] * 23000f));
            sid.samples.RemoveRange(0, output.Length);
        }
    }

    // WinMM streams short PCM buffers generated live by the managed SID emulator.
    internal sealed class SidAudioPlayer : IDisposable
    {
        [StructLayout(LayoutKind.Sequential, Pack = 2)]
        private struct WaveFormat
        {
            public ushort Format, Channels;
            public uint SampleRate, BytesPerSecond;
            public ushort BlockAlign, Bits, Extra;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct WaveHeader
        {
            public IntPtr Data;
            public uint Length, Recorded;
            public UIntPtr User;
            public uint Flags, Loops;
            public IntPtr Next;
            public UIntPtr Reserved;
        }
        [DllImport("winmm.dll")] private static extern uint waveOutOpen(out IntPtr device, uint id, ref WaveFormat format, IntPtr callback, IntPtr instance, uint flags);
        [DllImport("winmm.dll")] private static extern uint waveOutPrepareHeader(IntPtr device, IntPtr header, uint size);
        [DllImport("winmm.dll")] private static extern uint waveOutUnprepareHeader(IntPtr device, IntPtr header, uint size);
        [DllImport("winmm.dll")] private static extern uint waveOutWrite(IntPtr device, IntPtr header, uint size);
        [DllImport("winmm.dll")] private static extern uint waveOutReset(IntPtr device);
        [DllImport("winmm.dll")] private static extern uint waveOutClose(IntPtr device);
        private readonly ManualResetEvent stop = new(false);
        private Thread? worker;
        private bool disposed;
        internal event Action<Exception>? Failed;
        private static void Check(uint result)
        {
            if (result != 0) throw new InvalidOperationException("Windows audio error " + result);
        }

        internal void Start(Action<short[]>? fill = null)
        {
            fill ??= new IntroSidEngine().Fill;
            var generate = fill;
            worker = new Thread(() => Run(generate)) { IsBackground = true, Name = "KHARVOX intro audio" };
            worker.Start();
        }

        private void Run(Action<short[]> fill)
        {
            IntPtr device = IntPtr.Zero;
            var headers = new List<IntPtr>();
            var buffers = new List<IntPtr>();
            var prepared = new HashSet<IntPtr>();
            uint size = (uint)Marshal.SizeOf(typeof(WaveHeader));
            try
            {
                var format = new WaveFormat { Format = 1, Channels = 1, SampleRate = 44100, BytesPerSecond = 88200, BlockAlign = 2, Bits = 16 };
                Check(waveOutOpen(out device, uint.MaxValue, ref format, IntPtr.Zero, IntPtr.Zero, 0));
                var samples = new short[2048];
                for (int i = 0; i < 3; i++)
                {
                    var data = Marshal.AllocHGlobal(samples.Length * 2); buffers.Add(data);
                    var header = Marshal.AllocHGlobal((int)size); headers.Add(header);
                    Marshal.StructureToPtr(new WaveHeader { Data = data, Length = (uint)samples.Length * 2 }, header, false);
                    Check(waveOutPrepareHeader(device, header, size)); prepared.Add(header);
                    fill(samples); Marshal.Copy(samples, 0, data, samples.Length);
                    Check(waveOutWrite(device, header, size));
                }
                while (!stop.WaitOne(3))
                {
                    for (int i = 0; i < headers.Count; i++)
                    {
                        if (stop.WaitOne(0)) break;
                        var header = Marshal.PtrToStructure<WaveHeader>(headers[i]);
                        if ((header.Flags & 1) == 0) continue; // WHDR_DONE
                        fill(samples);
                        Marshal.Copy(samples, 0, buffers[i], samples.Length);
                        Check(waveOutWrite(device, headers[i], size));
                    }
                }
            }
            catch (Exception ex) { if (!stop.WaitOne(0)) Failed?.Invoke(ex); }
            finally
            {
                if (device != IntPtr.Zero) waveOutReset(device);
                foreach (var header in headers)
                {
                    if (prepared.Contains(header)) waveOutUnprepareHeader(device, header, size);
                    Marshal.FreeHGlobal(header);
                }
                foreach (var buffer in buffers) Marshal.FreeHGlobal(buffer);
                if (device != IntPtr.Zero) waveOutClose(device);
            }
        }
        public void Dispose()
        {
            if (disposed) return;
            disposed = true;
            stop.Set(); worker?.Join(); stop.Dispose();
        }
    }
}
