#nullable disable
// MIT License
// 
// Copyright (c) 2016 Yve Verstrepen
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Modified by Lasse Oorni for OldschoolEngine2

using System;


namespace EMU6502
{
#if EVENT
    public delegate void Cycle();
#endif
    public class MOS6502
    {
        public delegate void KernalTrap(ushort address);
        public event KernalTrap kernalTrap;

        readonly RAM64K _ram;
        public RAM64K RAM { get { return _ram; } }

        //readonly byte[] _bcdToDec;
        //readonly byte[] _decToBcd;

        byte _opcode;
        /// <summary>
        /// Last opcode
        /// </summary>
        public byte Opcode { get { return _opcode; } }
        byte _data;
        /// <summary>
        /// First byte after last opcode
        /// </summary>
        public byte OpcodeData { get { return _data; } }
        ushort _address;
        /// <summary>
        /// Combined address value after last opcode
        /// </summary>
        public ushort OpcodeAddress { get { return _address; } }

        // Registers
        byte _a;
        /// <summary>
        /// Accumulator
        /// </summary>
        public byte A { get { return _a; } set { _a = value; } }
        byte _x;
        /// <summary>
        /// Index register X
        /// </summary>
        public byte X { get { return _x; } set { _x = value; } }
        byte _y;
        /// <summary>
        /// Index register Y
        /// </summary>
        public byte Y { get { return _y; } set { _y = value; } }
        byte _sp;
        /// <summary>
        /// Stack Pointer
        /// </summary>
        public byte SP { get { return _sp; } }
        ushort _pc;
        /// <summary>
        /// Program Counter
        /// </summary>
        public ushort PC { get { return _pc; } }

        // Flags
        bool _carry;    //0x1
        bool _zero;     //0x2
        bool _interrupt;//0x4
        bool _decimal;  //0x8
        //bool _break;  //0x10 only exists on stack
        //bool _unused; //0x20
        bool _overflow; //0x40
        bool _negative; //0x80

        byte _status
        {
            get
            {
                return (byte)
                    ((_carry ? 0x1 : 0) |
                    (_zero ? 0x2 : 0) |
                    (_interrupt ? 0x4 : 0) |
                    (_decimal ? 0x8 : 0) |
                    0x10 | //(_break ? 0x10 : 0) |
                    0x20 |
                    (_overflow ? 0x40 : 0) |
                    (_negative ? 0x80 : 0));
            }
            set
            {
                _carry = (value & 0x1) != 0;
                _zero = (value & 0x2) != 0;
                _interrupt = (value & 0x4) != 0;
                _decimal = (value & 0x8) != 0;
                //_break = (value & 0x10) != 0;
                _overflow = (value & 0x40) != 0;
                _negative = (value & 0x80) != 0;
            }
        }

        /// <summary>
        /// Carry flag
        /// </summary>
        public bool Carry { get { return _carry; } }
        /// <summary>
        /// Zero flag
        /// </summary>
        public bool Zero { get { return _zero; } }
        /// <summary>
        /// Interrupt disable flag
        /// </summary>
        public bool Interrupt { get { return _interrupt; } }
        /// <summary>
        /// Decimal flag
        /// </summary>
        public bool Decimal { get { return _decimal; } }
        /// <summary>
        /// Overflow flag
        /// </summary>
        public bool Overflow { get { return _overflow; } }
        /// <summary>
        /// Sign flag
        /// </summary>
        public bool Negative { get { return _negative; } }

        /// <summary>
        /// Status register
        /// </summary>
        public byte Status { get { return _status; } }

        bool _nmi;
        bool _irq;
        bool _reset;

        bool _jam;
        /// <summary>
        /// Returns true if the cpu is jammed
        /// </summary>
        public bool Jam { get { return _jam; } }

        int _cycles;
        /// <summary>
        /// Cycle counter. Returns the amount of cycles since the last reset.
        /// </summary>
        public int Cycles { get { return _cycles; } set { _cycles = value; } }

#if EVENT
        /// <summary>
        /// Cycle event. Fires everytime a cycle is done.
        /// </summary>
        public event Cycle Cycle;
#endif

        public MOS6502(RAM64K ram)
        {
            /*
            // Generate BCD conversion tables
            _bcdToDec = new byte[0x100];
            _decToBcd = new byte[0x100];
            for (int i = 0; i <= 0xFF; i++)
            {
                _bcdToDec[i] = (byte)(i / 16 * 10 + i % 16);
                _decToBcd[i] = (byte)(i / 10 * 16 + i % 10);
            }
            */
            _ram = ram;
            Reset();
        }



        void CountCycle(int cycles = 1)
        {
#if EVENT
            if (Cycle != null)
            {
                while (cycles-- > 0)
                {
                    _cycles++;
                    Cycle();
                }
            }
            else
#endif
                _cycles += cycles;
        }

        /// <summary>
        /// Jumps to the specified address.
        /// </summary>
        /// <param name="address">The address to jump to.</param>
        public void Jump(ushort address)
        {
            _pc = _ram.Read16(address);
        }


        #region Helper Functions

        ushort Combine(byte a, byte b)
        {
            ushort value = b;
            value <<= 8;
            value |= a;
            return value;
        }

        void Push(byte data)
        {
            _ram.Write((ushort)(_sp | 0x0100), data);
            _sp--;
        }
        void Push16(ushort data)
        {
            Push((byte)(data >> 8));
            Push((byte)(data & 0xFF));
        }

        byte Pop()
        {
            _sp++;
            return _ram.Read((ushort)(_sp | 0x0100));
        }
        ushort Pop16()
        {
            byte a = Pop();
            byte b = Pop();
            return Combine(a, b);
        }

        void CheckPageBoundaries(ushort a, ushort b)
        {
            if ((a & 0xFF00) != (b & 0xFF00))
                CountCycle();
        }

        // Addressing modes
        /*
        ushort ZeroPage(byte argA)
        {
            return argA;
        }
        */
        ushort ZeroPageX(byte address)
        {
            return (ushort)((address + _x) & 0xFF);
        }
        ushort ZeroPageY(byte address)
        {
            return (ushort)((address + _y) & 0xFF);
        }
        /*
        ushort Absolute(byte argA, byte argB)
        {
            return Combine(argA, argB);
        }
        */
        ushort AbsoluteX(ushort address, bool checkPage = false)
        {
            //ushort address = Combine(addrA, addrB);
            ushort trAddress = (ushort)(address + _x);
            if (checkPage)
                CheckPageBoundaries(address, trAddress);
            return trAddress;
        }
        ushort AbsoluteY(ushort address, bool checkPage = false)
        {
            //ushort address = Combine(addrA, addrB);
            ushort trAddress = (ushort)(address + _y);
            if (checkPage)
                CheckPageBoundaries(address, trAddress);
            return trAddress;
        }
        ushort IndirectX(byte address)
        {
            return _ram.Read16((ushort)((address + _x) & 0xFF));
        }
        ushort IndirectY(byte address, bool checkPage = false)
        {
            ushort value = _ram.Read16(address);
            ushort translatedAddress = (ushort)(value + _y);
            if (checkPage)
                CheckPageBoundaries(value, translatedAddress);
            return translatedAddress;
        }

        #endregion


        #region Opcode Implementations

        void SetZN(byte value)
        {
            _zero = value == 0;
            _negative = (value & 0x80) != 0;
        }

        void ADC(byte value)
        {
            if (_decimal)
            {
                // Low nybble
                int low = (_a & 0xF) + (value & 0xF) + (_carry ? 0x1 : 0);
                bool halfCarry = (low > 0x9);

                // High nybble
                int high = (_a & 0xF0) + (value & 0xF0) + (halfCarry ? 0x10 : 0);
                _carry = (high > 0x9F);

                // Set flags on the binary result
                byte binary = (byte)((low & 0xF) + (high & 0xF0));
                SetZN(binary);
                _overflow = ((_a ^ binary) & (value ^ binary) & 0x80) != 0;
                //_overflow = ((_a ^ value) & 0x80) == 0 && binary > 127 && binary < 0x180;

                // Decimal adjust
                if (halfCarry)
                    low += 0x6;
                if (_carry)
                    high += 0x60;

                _a = (byte)((low & 0xF) + (high & 0xF0));
            }
            else
            {
                int result = _a + value + (_carry ? 1 : 0);
                _overflow = ((_a ^ result) & (value ^ result) & 0x80) != 0;
                _carry = result > 0xFF;
                _a = (byte)result;
                SetZN(_a);
            }
        }
        void ADC(ushort address)
        {
            ADC(_ram.Read(address));
        }

        void AND(byte value)
        {
            _a = (byte)(_a & value);
            SetZN(_a);
        }
        void AND(ushort address)
        {
            AND(_ram.Read(address));
        }

        byte ASL(byte value)
        {
            _carry = (value & 0x80) != 0;
            value <<= 1;
            SetZN(value);
            return value;
        }
        void ASL(ushort address)
        {
            byte value = _ram.Read(address);
            _ram.Write(address, ASL(value));
        }

        void Bxx(byte value)
        {
            ushort address = (ushort)(_pc + (sbyte)value);
            CheckPageBoundaries(_pc, address);
            _pc = address;
            CountCycle();
        }

        void BIT(ushort address)
        {
            byte value = _ram.Read(address);
            _overflow = (value & 0x40) != 0;
            _negative = (value & 0x80) != 0;
            value &= _a;
            _zero = value == 0;
        }

        void Cxx(byte value, byte register)
        {
            _carry = (register >= value);
            value = (byte)(register - value);
            SetZN(value);
        }
        void Cxx(ushort address, byte register)
        {
            Cxx(_ram.Read(address), register);
        }
        
        byte Dxx(byte value)
        {
            value--;
            SetZN(value);
            return value;
        }
        void DEC(ushort address)
        {
            byte value = _ram.Read(address);
            _ram.Write(address, Dxx(value));
        }

        void EOR(byte value)
        {
            _a ^= value;
            SetZN(_a);
        }
        void EOR(ushort address)
        {
            byte value = _ram.Read(address);
            EOR(value);
        }

        byte Ixx(byte value)
        {
            value++;
            SetZN(value);
            return value;
        }
        void INC(ushort address)
        {
            byte value = _ram.Read(address);
            _ram.Write(address, Ixx(value));
        }

        void LDA(byte value)
        {
            _a = value;
            SetZN(value);
        }
        void LDA(ushort address)
        {
            LDA(_ram.Read(address));
        }
        void LDX(byte value)
        {
            _x = value;
            SetZN(value);
        }
        void LDX(ushort address)
        {
            LDX(_ram.Read(address));
        }
        void LDY(byte value)
        {
            _y = value;
            SetZN(value);
        }
        void LDY(ushort address)
        {
            LDY(_ram.Read(address));
        }

        byte LSR(byte value)
        {
            _carry = (value & 0x1) != 0;
            value >>= 1;
            SetZN(value);
            return value;
        }
        void LSR(ushort address)
        {
            byte value = _ram.Read(address);
            _ram.Write(address, LSR(value));
        }

        void ORA(byte value)
        {
            _a |= value;
            SetZN(_a);
        }
        void ORA(ushort address)
        {
            ORA(_ram.Read(address));
        }

        byte ROL(byte value)
        {
            bool oldCarry = _carry;
            _carry = (value & 0x80) != 0;
            value <<= 1;
            if (oldCarry) value |= 0x1;
            SetZN(value);
            return value;
        }
        void ROL(ushort address)
        {
            byte value = _ram.Read(address);
            _ram.Write(address, ROL(value));
        }

        byte ROR(byte value)
        {
            bool oldCarry = _carry;
            _carry = (value & 0x1) != 0;
            value >>= 1;
            if (oldCarry) value |= 0x80;
            SetZN(value);
            return value;
        }
        void ROR(ushort address)
        {
            byte value = _ram.Read(address);
            _ram.Write(address, ROR(value));
        }

        void SBC(byte value)
        {
            if (_decimal)
            {
                // Low nybble
                int low = 0xF + (_a & 0xF) - (value & 0xF) + (_carry ? 0x1 : 0);
                bool halfCarry = (low > 0xF);

                // High nybble
                int high = 0xF0 + (_a & 0xF0) - (value & 0xF0) + (halfCarry ? 0x10 : 0);
                _carry = (high > 0xFF);

                // Set flags on the binary result
                byte binary = (byte)((low & 0xF) + (high & 0xF0));
                SetZN(binary);
                _overflow = ((_a ^ binary) & (~value ^ binary) & 0x80) != 0;
                //_overflow = ((_a ^ value) & 0x80) != 0 && result >= 0x80 && result < 0x180;

                // Decimal adjust
                if (!halfCarry)
                    low -= 0x6;
                if (!_carry)
                    high -= 0x60;

                _a = (byte)((low & 0xF) + (high & 0xF0));
            }
            else
            {
                int result = 0xFF + _a - value + (_carry ? 1 : 0);
                _overflow = ((_a ^ result) & (~value ^ result) & 0x80) != 0;
                _carry = result > 0xFF;
                _a = (byte)result;
                SetZN(_a);
            }
        }
        void SBC(ushort address)
        {
            SBC(_ram.Read(address));
        }
        
        #endregion


        public void SetNMI()
        {
            _nmi = true;
        }

        public void SetIRQ(bool value)
        {
            _irq = value;
        }

        /// <summary>
        /// Resets the CPU.
        /// </summary>
        public void Reset()
        {
            _reset = true;
        }

        /// <summary>
        /// Execute the next opcode.
        /// </summary>
        public void Process()
        {
            if (_reset)
            {
                // Writes are ignored at reset, so don't push anything but do modify the stack pointer
                _sp -= 3;
                _interrupt = true;
                _pc = _ram.Read16(0xFFFC);
                _cycles = 0;
                CountCycle(7);
                _reset = false;
                _jam = false;
                _nmi = false;
                _irq = false;
                return;
            }

            if (_jam)
            {
                _nmi = false;
                _irq = false;
                return;
            }

            if (_nmi)
            {
                Push16(_pc);
                Push((byte)(_status & 0xEF)); // Mask off break flag
                _interrupt = true;
                _pc = _ram.Read16(0xFFFA);
                CountCycle(7);
                _nmi = false;
                _irq = false;
                return;
            }
            else if (_irq && !_interrupt)
            {
                Push16(_pc);
                Push((byte)(_status & 0xEF)); // Mask off break flag
                _interrupt = true;

                if ((_ram.Read(0x1) & 0x2) != 0x0) // If Kernal ROM on, use soft vector and push CPU registers 
                {
                    _pc = _ram.Read16(0x0314);
                    Push(_a);
                    Push(_x);
                    Push(_y);
                }
                else
                    _pc = _ram.Read16(0xFFFE);

                _irq = false;
                return;
            }

            // HACK: if Kernal + Basic on, and code jumps to $a8bc, re-init SYS address from Basic program RAM beginning
            byte bankBits = (byte)(_ram.Read(0x1) & 0x3);

            if (_pc == 0xa8bc && bankBits == 0x3)
            {
                ushort address = 0x0801;
                ushort sysAddress = 0;
                // Scan for SYS address
                while (address < 0xffff && (_ram[address] < 0x30 || _ram[address] > 0x39))
                    ++address;
                while (address < 0xffff && _ram[address] >= 0x30 && _ram[address] <= 0x39)
                {
                    sysAddress *= 10;
                    sysAddress += (byte)(_ram[address] - 0x30);
                    ++address;
                }

                _pc = sysAddress;

                System.Diagnostics.Debug.WriteLine("Re-init SYS address (crude Basic emulation) to " + _pc);
            }

            _opcode = _ram.Read(_pc);
            _data = _ram.Read((ushort)(_pc + 1));
            _address = Combine(_data, _ram.Read((ushort)(_pc + 2)));

            if (bankBits >= 0x2)
            {
                // HACK: emulate $ea81 interrupt termination if Kernal on
                if (_pc == 0xea81)
                {
                    _y = Pop();
                    _x = Pop();
                    _a = Pop();
                    _opcode = 0x40; // RTI
                }
                // Rest of Kernal emulation; simply return from unhandled routines
                else if (_pc >= 0xe000)
                {
                    if (kernalTrap != null)
                        kernalTrap(_pc);

                    _opcode = 0x60; // RTS, return from the Kernal routine
                }
            }
            
            switch (_opcode)
            {
                // ADC
                case (0x69): // Immediate
                    _pc += 2;
                    ADC(_data);
                    CountCycle(2);
                    break;
                case (0x65): // Zero Page
                    _pc += 2;
                    ADC((ushort)_data);
                    CountCycle(3);
                    break;
                case (0x75): // Zero Page,X
                    _pc += 2;
                    ADC(ZeroPageX(_data));
                    CountCycle(4);
                    break;
                case (0x6D): // Absolute
                    _pc += 3;
                    ADC(_address);
                    CountCycle(4);
                    break;
                case (0x7D): // Absolute,X
                    _pc += 3;
                    ADC(AbsoluteX(_address, true));
                    CountCycle(4);
                    break;
                case (0x79): // Absolute,Y
                    _pc += 3;
                    ADC(AbsoluteY(_address, true));
                    CountCycle(4);
                    break;
                case (0x61): // Indirect,X
                    _pc += 2;
                    ADC(IndirectX(_data));
                    CountCycle(6);
                    break;
                case (0x71): // Indirect,Y
                    _pc += 2;
                    ADC(IndirectY(_data, true));
                    CountCycle(5);
                    break;

                // AND
                case (0x29): // Immediate
                    _pc += 2;
                    AND(_data);
                    CountCycle(2);
                    break;
                case (0x25): // Zero Page
                    _pc += 2;
                    AND((ushort)_data);
                    CountCycle(2);
                    break;
                case (0x35): // Zero Page X
                    _pc += 2;
                    AND(ZeroPageX(_data));
                    CountCycle(3);
                    break;
                case (0x2D): // Absolute
                    _pc += 3;
                    AND(_address);
                    CountCycle(4);
                    break;
                case (0x3D): // Absolute,X
                    _pc += 3;
                    AND(AbsoluteX(_address, true));
                    CountCycle(4);
                    break;
                case (0x39): // Absolute,Y
                    _pc += 3;
                    AND(AbsoluteY(_address, true));
                    CountCycle(4);
                    break;
                case (0x21): // Indirect,X
                    _pc += 2;
                    AND(IndirectX(_data));
                    CountCycle(6);
                    break;
                case (0x31): // Indirect,Y
                    _pc += 2;
                    AND(IndirectY(_data, true));
                    CountCycle(5);
                    break;

                case (0x0A): // Accumulator
                    _pc += 1;
                    _a = ASL(_a);
                    CountCycle(2);
                    break;
                case (0x06): // Zero Page
                    _pc += 2;
                    ASL((ushort)_data);
                    CountCycle(5);
                    break;
                case (0x16): // Zero Page,X
                    _pc += 2;
                    ASL(ZeroPageX(_data));
                    CountCycle(6);
                    break;
                case (0x0E): // Absolute
                    _pc += 3;
                    ASL(_address);
                    CountCycle(6);
                    break;
                case (0x1E): // Absolute,X
                    _pc += 3;
                    ASL(AbsoluteX(_address));
                    CountCycle(7);
                    break;

                // BCC
                case (0x90): // Relative
                    _pc += 2;
                    if (!_carry)
                        Bxx(_data);
                    CountCycle(2);
                    break;

                // BCS
                case (0xB0): // Relative
                    _pc += 2;
                    if (_carry)
                        Bxx(_data);
                    CountCycle(2);
                    break;

                // BEQ
                case (0xF0): // Relative
                    _pc += 2;
                    if (_zero)
                        Bxx(_data);
                    CountCycle(2);
                    break;

                // BIT
                case (0x24): // Zero Page
                    _pc += 2;
                    BIT((ushort)_data);
                    CountCycle(3);
                    break;
                case (0x2C): // Absolute
                    _pc += 3;
                    BIT(_address);
                    CountCycle(4);
                    break;

                // BMI
                case (0x30): // Relative
                    _pc += 2;
                    if (_negative)
                        Bxx(_data);
                    CountCycle(2);
                    break;

                // BNE
                case (0xD0): // Relative
                    _pc += 2;
                    if (!_zero)
                        Bxx(_data);
                    CountCycle(2);
                    break;

                // BPL
                case (0x10): // Relative
                    _pc += 2;
                    if (!_negative)
                        Bxx(_data);
                    CountCycle(2);
                    break;
                    
                // BRK
                case (0x00): // Implied
                    if (_pc != 0)
                        System.Diagnostics.Debug.WriteLine("BRK caused at " + _pc);
                    _pc += 2;
#if BUG
                    if (_irq || _nmi || _reset) break; // Emulate the interrupt bug
#endif
                    Push16(_pc);
                    Push(_status);
                    _interrupt = true;
                    _pc = _ram.Read16(0xFFFE);
                    CountCycle(7);
                    break;
                    
                // BVC
                case (0x50): // Relative
                    _pc += 2;
                    if (!_overflow)
                        Bxx(_data);
                    CountCycle(2);
                    break;

                // BVS
                case (0x70): // Relative
                    _pc += 2;
                    if (_overflow)
                        Bxx(_data);
                    CountCycle(2);
                    break;

                // CLC
                case (0x18): // Implied
                    _pc += 1;
                    _carry = false;
                    CountCycle(2);
                    break;
                
                // CLD
                case (0xD8): // Implied
                    _pc += 1;
                    _decimal = false;
                    CountCycle(2);
                    break;
                    
                // CLI
                case (0x58): // Implied
                    _pc += 1;
                    _interrupt = false;
                    CountCycle(2);
                    break;
                    
                // CLV
                case (0xB8): // Implied
                    _pc += 1;
                    _overflow = false;
                    CountCycle(2);
                    break;
                    
                // CMP
                case (0xC9): // Immediate
                    _pc += 2;
                    Cxx(_data, _a);
                    CountCycle(2);
                    break;
                case (0xC5): // Zero Page
                    _pc += 2;
                    Cxx((ushort)_data, _a);
                    CountCycle(3);
                    break;
                case (0xD5): // Zero Page,X
                    _pc += 2;
                    Cxx(ZeroPageX(_data), _a);
                    CountCycle(4);
                    break;
                case (0xCD): // Absolute
                    _pc += 3;
                    Cxx(_address, _a);
                    CountCycle(4);
                    break;
                case (0xDD): // Absolute,X
                    _pc += 3;
                    Cxx(AbsoluteX(_address, true), _a);
                    CountCycle(4);
                    break;
                case (0xD9): // Absolute,Y
                    _pc += 3;
                    Cxx(AbsoluteY(_address, true), _a);
                    CountCycle(4);
                    break;
                case (0xC1): // Indirect,X
                    _pc += 2;
                    Cxx(IndirectX(_data), _a);
                    CountCycle(6);
                    break;
                case (0xD1): // Indirect,Y
                    _pc += 2;
                    Cxx(IndirectY(_data, true), _a);
                    CountCycle(5);
                    break;
                    
                // CPX
                case (0xE0): // Immediate
                    _pc += 2;
                    Cxx(_data, _x);
                    CountCycle(2);
                    break;
                case (0xE4): // Zero Page
                    _pc += 2;
                    Cxx((ushort)_data, _x);
                    CountCycle(3);
                    break;
                case (0xEC): // Absolute
                    _pc += 3;
                    Cxx(_address, _x);
                    CountCycle(4);
                    break;

                // CPY
                case (0xC0): // Immediate
                    _pc += 2;
                    Cxx(_data, _y);
                    CountCycle(2);
                    break;
                case (0xC4): // Zero Page
                    _pc += 2;
                    Cxx((ushort)_data, _y);
                    CountCycle(3);
                    break;
                case (0xCC): // Absolute
                    _pc += 3;
                    Cxx(_address, _y);
                    CountCycle(4);
                    break;

                // DEC
                case (0xC6): // Zero Page
                    _pc += 2;
                    DEC((ushort)_data);
                    CountCycle(5);
                    break;
                case (0xD6): // Zero Page,X
                    _pc += 2;
                    DEC(ZeroPageX(_data));
                    CountCycle(6);
                    break;
                case (0xCE): // Absolute
                    _pc += 3;
                    DEC(_address);
                    CountCycle(6);
                    break;
                case (0xDE): // Absolute,X
                    _pc += 3;
                    DEC(AbsoluteX(_address));
                    CountCycle(7);
                    break;

                // DEX
                case (0xCA): // Implied
                    _pc += 1;
                    _x = Dxx(_x);
                    CountCycle(2);
                    break;

                // DEY
                case (0x88): // Implied
                    _pc += 1;
                    _y = Dxx(_y);
                    CountCycle(2);
                    break;

                // EOR
                case (0x49): // Immediate
                    _pc += 2;
                    EOR(_data);
                    CountCycle(2);
                    break;
                case (0x45): // Zero Page
                    _pc += 2;
                    EOR((ushort)_data);
                    CountCycle(3);
                    break;
                case (0x55): // Zero Page,X
                    _pc += 2;
                    EOR(ZeroPageX(_data));
                    CountCycle(4);
                    break;
                case (0x4D): // Absolute
                    _pc += 3;
                    EOR(_address);
                    CountCycle(4);
                    break;
                case (0x5D): // Absolute,X
                    _pc += 3;
                    EOR(AbsoluteX(_address, true));
                    CountCycle(4);
                    break;
                case (0x59): // Absolute,Y
                    _pc += 3;
                    EOR(AbsoluteY(_address, true));
                    CountCycle(4);
                    break;
                case (0x41): // Indirect,X
                    _pc += 2;
                    EOR(IndirectX(_data));
                    CountCycle(6);
                    break;
                case (0x51): // Indirect,Y
                    _pc += 2;
                    EOR(IndirectY(_data, true));
                    CountCycle(5);
                    break;

                // INC
                case (0xE6): // Zero Page
                    _pc += 2;
                    INC((ushort)_data);
                    CountCycle(5);
                    break;
                case (0xF6): // Zero Page,X
                    _pc += 2;
                    INC(ZeroPageX(_data));
                    CountCycle(6);
                    break;
                case (0xEE): // Absolute
                    _pc += 3;
                    INC(_address);
                    CountCycle(6);
                    break;
                case (0xFE): // Absolute,X
                    _pc += 3;
                    INC(AbsoluteX(_address));
                    CountCycle(7);
                    break;

                // INX
                case (0xE8): // Implied
                    _pc += 1;
                    _x = Ixx(_x);
                    CountCycle(2);
                    break;

                // INY
                case (0xC8): // Implied
                    _pc += 1;
                    _y = Ixx(_y);
                    CountCycle(2);
                    break;

                // JMP
                case (0x4C): // Absolute
                    _pc = _address;
                    CountCycle(3);
                    break;
                case (0x6C): // Indirect
                    ushort address = _address;
#if BUG
                    if ((address & 0x00FF) == 0x00FF) // Emulate the indirect jump bug
                        _pc = (ushort)((_ram.Read((ushort)(address & 0xFF00)) << 8) | _ram.Read(address));
                    else
#endif
                    _pc = _ram.Read16(address);
                    CountCycle(5);
                    break;

                // JSR
                case (0x20): // Absolute
                    Push16((ushort)(_pc + 2)); // + 3 - 1
                    _pc = _address;
                    CountCycle(6);
                    break;

                // LDA
                case (0xA9): // Immediate
                    _pc += 2;
                    LDA(_data);
                    CountCycle(2);
                    break;
                case (0xA5): // Zero Page
                    _pc += 2;
                    LDA((ushort)_data);
                    CountCycle(3);
                    break;
                case (0xB5): // Zero Page,X
                    _pc += 2;
                    LDA(ZeroPageX(_data));
                    CountCycle(4);
                    break;
                case (0xAD): // Absolute
                    _pc += 3;
                    LDA(_address);
                    CountCycle(4);
                    break;
                case (0xBD): // Absolute,X
                    _pc += 3;
                    LDA(AbsoluteX(_address, true));
                    CountCycle(4);
                    break;
                case (0xB9): // Absolute,Y
                    _pc += 3;
                    LDA(AbsoluteY(_address, true));
                    CountCycle(4);
                    break;
                case (0xA1): // Indirect,X
                    _pc += 2;
                    LDA(IndirectX(_data));
                    CountCycle(6);
                    break;
                case (0xB1): // Indirect,Y
                    _pc += 2;
                    LDA(IndirectY(_data, true));
                    CountCycle(5);
                    break;

                // LDX
                case (0xA2): // Immediate
                    _pc += 2;
                    LDX(_data);
                    CountCycle(2);
                    break;
                case (0xA6): // Zero Page
                    _pc += 2;
                    LDX((ushort)_data);
                    CountCycle(3);
                    break;
                case (0xB6): // Zero Page,Y
                    _pc += 2;
                    LDX(ZeroPageY(_data));
                    CountCycle(4);
                    break;
                case (0xAE): // Absolute
                    _pc += 3;
                    LDX(_address);
                    CountCycle(4);
                    break;
                case (0xBE): // Absolute,Y
                    _pc += 3;
                    LDX(AbsoluteY(_address, true));
                    CountCycle(4);
                    break;
                    
                // LDY
                case (0xA0): // Immediate
                    _pc += 2;
                    LDY(_data);
                    CountCycle(2);
                    break;
                case (0xA4): // Zero Page
                    _pc += 2;
                    LDY((ushort)_data);
                    CountCycle(3);
                    break;
                case (0xB4): // Zero Page,X
                    _pc += 2;
                    LDY(ZeroPageX(_data));
                    CountCycle(4);
                    break;
                case (0xAC): // Absolute
                    _pc += 3;
                    LDY(_address);
                    CountCycle(4);
                    break;
                case (0xBC): // Absolute,X
                    _pc += 3;
                    LDY(AbsoluteX(_address, true));
                    CountCycle(4);
                    break;
                    
                // LSR
                case (0x4A): // Accumulator
                    _pc += 1;
                    _a = LSR(_a);
                    CountCycle(2);
                    break;
                case (0x46): // Zero Page
                    _pc += 2;
                    LSR((ushort)_data);
                    CountCycle(5);
                    break;
                case (0x56): // Zero Page,X
                    _pc += 2;
                    LSR(ZeroPageX(_data));
                    CountCycle(6);
                    break;
                case (0x4E): // Absolute
                    _pc += 3;
                    LSR(_address);
                    CountCycle(6);
                    break;
                case (0x5E): // Absolute,X
                    _pc += 3;
                    LSR(AbsoluteX(_address));
                    CountCycle(7);
                    break;

                // NOP
                case(0xEA): // Implied
                    _pc += 1;
                    CountCycle(2);
                    break;
                    
                // ORA
                case (0x09): // Immediate
                    _pc += 2;
                    ORA(_data);
                    CountCycle(2);
                    break;
                case (0x05): // Zero Page
                    _pc += 2;
                    ORA((ushort)_data);
                    CountCycle(3);
                    break;
                case (0x15): // Zero Page,X
                    _pc += 2;
                    ORA(ZeroPageX(_data));
                    CountCycle(4);
                    break;
                case (0x0D): // Absolute
                    _pc += 3;
                    ORA(_address);
                    CountCycle(4);
                    break;
                case (0x1D): // Absolute,X
                    _pc += 3;
                    ORA(AbsoluteX(_address, true));
                    CountCycle(4);
                    break;
                case (0x19): // Absolute,Y
                    _pc += 3;
                    ORA(AbsoluteY(_address, true));
                    CountCycle(4);
                    break;
                case (0x01): // Indirect,X
                    _pc += 2;
                    ORA(IndirectX(_data));
                    CountCycle(6);
                    break;
                case (0x11): // Indirect,Y
                    _pc += 2;
                    ORA(IndirectY(_data, true));
                    CountCycle(5);
                    break;

                // PHA
                case (0x48): // Implied
                    _pc += 1;
                    Push(_a);
                    CountCycle(3);
                    break;

                // PHP
                case (0x08): // Implied
                    _pc += 1;
                    Push(_status);
                    CountCycle(3);
                    break;

                // PLA
                case (0x68): // Implied
                    _pc += 1;
                    _a = Pop();
                    SetZN(_a);
                    CountCycle(4);
                    break;

                // PLP
                case (0x28): // Implied
                    _pc += 1;
                    _status = Pop();
                    CountCycle(4);
                    break;

                // ROL
                case (0x2A): // Accumulator
                    _pc += 1;
                    _a = ROL(_a);
                    CountCycle(2);
                    break;
                case (0x26): // Zero Page
                    _pc += 2;
                    ROL((ushort)_data);
                    CountCycle(5);
                    break;
                case (0x36): // Zero Page,X
                    _pc += 2;
                    ROL(ZeroPageX(_data));
                    CountCycle(6);
                    break;
                case (0x2E): // Absolute
                    _pc += 3;
                    ROL(_address);
                    CountCycle(6);
                    break;
                case (0x3E): // Absolute,X
                    _pc += 3;
                    ROL(AbsoluteX(_address));
                    CountCycle(7);
                    break;

                // ROR
                case (0x6A): // Accumulator
                    _pc += 1;
                    _a = ROR(_a);
                    CountCycle(2);
                    break;
                case (0x66): // Zero Page
                    _pc += 2;
                    ROR((ushort)_data);
                    CountCycle(5);
                    break;
                case (0x76): // Zero Page,X
                    _pc += 2;
                    ROR(ZeroPageX(_data));
                    CountCycle(6);
                    break;
                case (0x6E): // Absolute
                    _pc += 3;
                    ROR(_address);
                    CountCycle(6);
                    break;
                case (0x7E): // Absolute,X
                    _pc += 3;
                    ROR(AbsoluteX(_address));
                    CountCycle(7);
                    break;

                // RTI
                case (0x40): // Implied
                    _status = Pop();
                    _pc = Pop16();
                    CountCycle(6);
                    break;

                // RTS
                case (0x60): // Implied
                    _pc = Pop16();
                    _pc += 1;
                    CountCycle(6);
                    break;
                    
                // SBC
                case (0xE9): // Immediate
                    _pc += 2;
                    SBC(_data);
                    CountCycle(2);
                    break;
                case (0xE5): // Zero Page
                    _pc += 2;
                    SBC((ushort)_data);
                    CountCycle(3);
                    break;
                case (0xF5): // Zero Page,X
                    _pc += 2;
                    SBC(ZeroPageX(_data));
                    CountCycle(4);
                    break;
                case (0xED): // Absolute
                    _pc += 3;
                    SBC(_address);
                    CountCycle(4);
                    break;
                case (0xFD): // Absolute,X
                    _pc += 3;
                    SBC(AbsoluteX(_address, true));
                    CountCycle(4);
                    break;
                case (0xF9): // Absolute,Y
                    _pc += 3;
                    SBC(AbsoluteY(_address, true));
                    CountCycle(4);
                    break;
                case (0xE1): // Indirect,X
                    _pc += 2;
                    SBC(IndirectX(_data));
                    CountCycle(6);
                    break;
                case (0xF1): // Indirect,Y
                    _pc += 2;
                    SBC(IndirectY(_data, true));
                    CountCycle(5);
                    break;

                // SEC
                case (0x38): // Implied
                    _pc += 1;
                    _carry = true;
                    CountCycle(2);
                    break;

                // SED
                case (0xF8): // Implied
                    _pc += 1;
                    _decimal = true;
                    CountCycle(2);
                    break;

                // SEI
                case (0x78): // Implied
                    _pc += 1;
                    _interrupt = true;
                    CountCycle(2);
                    break;

                // STA
                case (0x85): // Zero Page
                    _pc += 2;
                    _ram.Write((ushort)_data, _a);
                    CountCycle(3);
                    break;
                case (0x95): // Zero Page,X
                    _pc += 2;
                    _ram.Write(ZeroPageX(_data), _a);
                    CountCycle(4);
                    break;
                case (0x8D): // Absolute
                    _pc += 3;
                    _ram.Write(_address, _a);
                    CountCycle(4);
                    break;
                case (0x9D): // Absolute,X
                    _pc += 3;
                    _ram.Write(AbsoluteX(_address), _a);
                    CountCycle(5);
                    break;
                case (0x99): // Absolute,Y
                    _pc += 3;
                    _ram.Write(AbsoluteY(_address), _a);
                    CountCycle(5);
                    break;
                case (0x81): // Indirect,X
                    _pc += 2;
                    _ram.Write(IndirectX(_data), _a);
                    CountCycle(6);
                    break;
                case (0x91): // Indirect,Y
                    _pc += 2;
                    _ram.Write(IndirectY(_data), _a);
                    CountCycle(6);
                    break;

                // STX
                case (0x86): // Zero Page
                    _pc += 2;
                    _ram.Write((ushort)_data, _x);
                    CountCycle(3);
                    break;
                case (0x96): // Zero Page,Y
                    _pc += 2;
                    _ram.Write(ZeroPageY(_data), _x);
                    CountCycle(4);
                    break;
                case (0x8E): // Absolute
                    _pc += 3;
                    _ram.Write(_address, _x);
                    CountCycle(4);
                    break;

                // STY
                case (0x84): // Zero Page
                    _pc += 2;
                    _ram.Write((ushort)_data, _y);
                    CountCycle(3);
                    break;
                case (0x94): // Zero Page,X
                    _pc += 2;
                    _ram.Write(ZeroPageX(_data), _y);
                    CountCycle(4);
                    break;
                case (0x8C): // Absolute
                    _pc += 3;
                    _ram.Write(_address, _y);
                    CountCycle(4);
                    break;

                // TAX
                case (0xAA): // Implied
                    _pc += 1;
                    _x = _a;
                    SetZN(_x);
                    CountCycle(2);
                    break;

                // TAY
                case (0xA8): // Implied
                    _pc += 1;
                    _y = _a;
                    SetZN(_y);
                    CountCycle(2);
                    break;

                // TSX
                case (0xBA): // Implied
                    _pc += 1;
                    _x = _sp;
                    SetZN(_x);
                    CountCycle(2);
                    break;
                    
                // TXA
                case (0x8A): // Implied
                    _pc += 1;
                    _a = _x;
                    SetZN(_a);
                    CountCycle(2);
                    break;

                // TXS
                case (0x9A): // Implied
                    _pc += 1;
                    _sp = _x;
                    CountCycle(2);
                    break;

                // TYA
                case (0x98): // Implied
                    _pc += 1;
                    _a = _y;
                    SetZN(_a);
                    CountCycle(2);
                    break;

                /////////////////////
                // Illegal opcodes //
                /////////////////////
                
                // ANC
                case (0x0B): // Immediate
                case (0x2B):
                    _pc += 2;
                    AND(_data);
                    _carry = _negative;
                    CountCycle(2);
                    break;

                // KIL
                case (0x02):
                case (0x12):
                case (0x22):
                case (0x32):
                case (0x42):
                case (0x52):
                case (0x62):
                case (0x72):
                case (0x92):
                case (0xB2):
                case (0xD2):
                case (0xF2):
                    _jam = true;
                    break;
                
                //TODO: Count extra NOP cycles and double/triple bytes
                /*
                // NOP
                case (0x80):
                case (0x82):
                case (0xC2):
                case (0xE2):
                case (0x04):
                case (0x44):
                case (0x64):
                case (0x89):
                case (0x0C):
                case (0x14):
                case (0x34):
                case (0x54):
                case (0x74):
                case (0xD4):
                case (0xF4):
                case (0x1A):
                case (0x3A):
                case (0x5A):
                case (0x7A):
                case (0xDA):
                case (0xFA):
                case (0x1C):
                case (0x3C):
                case (0x5C):
                case (0x7C):
                case (0xDC):
                case (0xFC):
                    goto case (0xEA);
                    */
                default:
                    Console.WriteLine("Illegal opcode: " + _opcode.ToString("X2"));
                    goto case (0xEA); //NOP
            }

        }

    }

}