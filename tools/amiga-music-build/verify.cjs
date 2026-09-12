// Compare C# synthesis captured by the test harness to the source Amiga mixer.
const fs=require('fs'),vm=require('vm'),path=require('path');
const context={window:{},AudioContext:function(){this.sampleRate=44100;},console};
vm.createContext(context);
for(const name of ['Core.js','Amiga.js','S2Player.js']) vm.runInContext(fs.readFileSync(path.join(__dirname,name),'utf8'),context);
const source=fs.readFileSync(path.join(__dirname,'../../assets/cracktro/amiga/possessed.sid2'));
const stream=context.ByteArray(source.buffer.slice(source.byteOffset,source.byteOffset+source.byteLength));stream.endian=0;
const player=context.window.neoart.S2Player();player.loader(stream);player.initialize();player.stereo=0;player.volume=1;
const actual=fs.readFileSync(process.argv[2]);const count=(actual.length-44)/2;
let offset=0,maxError=0,totalError=0;
while(offset<count){
  const channels=[new Float32Array(8192),new Float32Array(8192)];
  player.mixer.fast({outputBuffer:{getChannelData:i=>channels[i]}});
  for(let i=0;i<8192&&offset<count;i++,offset++){
    const expected=Math.trunc(Math.max(-32768,Math.min(32767,channels[0][i]*30000)));
    const error=Math.abs(expected-actual.readInt16LE(44+offset*2));
    maxError=Math.max(maxError,error);totalError+=error;
  }
}
console.log(`Paula source comparison: ${count} samples, max error ${maxError}, mean ${totalError/count}.`);
if(maxError>1)throw new Error('C# playback differs from reference mixer');
