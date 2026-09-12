// Build-time SIDMON II -> Paula register sequence. No JavaScript ships in the EXE.
const fs = require('fs'), vm = require('vm'), path = require('path'), zlib = require('zlib');
const context = { window: {}, AudioContext: function() { this.sampleRate = 44100; }, console };
vm.createContext(context);
for (const name of ['Core.js', 'Amiga.js', 'S2Player.js']) vm.runInContext(fs.readFileSync(path.join(__dirname, name), 'utf8'), context);
const mixer = context.Amiga();
let events = [];
for (let channel = 0; channel < 4; channel++) {
  const state = [0,0,0,50,0], c = { next: null, initialize() { state.splice(0,5,0,0,0,50,0); } };
  for (const [reg, name] of ['enabled','pointer','length','period','volume'].entries()) {
    Object.defineProperty(c,name,{get:()=>state[reg],set:value=>{state[reg]=value;events.push([channel*5+reg,value]);}});
  }
  mixer.channels[channel] = c;
  if (channel) mixer.channels[channel-1].next = c;
}
const player = context.window.neoart.S2Player(mixer);
const source = fs.readFileSync(path.join(__dirname,'../../assets/cracktro/amiga/possessed.sid2'));
const stream=context.ByteArray(source.buffer.slice(source.byteOffset,source.byteOffset+source.byteLength));
stream.endian=0;
player.loader(stream);
if (player.version !== 2) throw new Error('SIDMON II load failed');
player.initialize();
const initial = Buffer.from(mixer.memory.map(v=>v&255));
let previous = Buffer.from(initial), ticks = [], remaining = -1;
for (let tick = 0; tick < 18000; tick++) {
  events = [];
  player.process();
  const patches = [];
  for(let i=0;i<mixer.memory.length;i++) {
    const value=mixer.memory[i]&255;
    if(value!==previous[i]) {patches.push([i,value]);previous[i]=value;}
  }
  ticks.push({patches,events});
  if (mixer.completed && remaining < 0) remaining=player.speed;
  if (remaining >= 0 && --remaining === 0) break;
}
if (remaining !== 0) throw new Error('Could not determine module loop');
const chunks=[];
function u32(v){const b=Buffer.alloc(4);b.writeUInt32LE(v>>>0);chunks.push(b);}
function u16(v){const b=Buffer.alloc(2);b.writeUInt16LE(v);chunks.push(b);}
function u8(v){chunks.push(Buffer.from([v]));}
chunks.push(Buffer.from('KPA1'));u32(initial.length);u32(ticks.length);chunks.push(initial);
for(const tick of ticks){
  u16(tick.patches.length);for(const [offset,value] of tick.patches){u32(offset);u8(value);}
  u16(tick.events.length);for(const [id,value] of tick.events){u8(id);u32(value);}
}
const packed=zlib.gzipSync(Buffer.concat(chunks),{level:9});
fs.writeFileSync(path.join(__dirname,'../../assets/cracktro/amiga/possessed.paula.gz'),packed);
console.log(`Compiled ${ticks.length} PAL ticks (${ticks.length/50}s), ${initial.length} sample bytes, ${packed.length} packed bytes.`);
