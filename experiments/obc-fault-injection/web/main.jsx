import React, {useEffect,useRef,useState} from 'react';
import {createRoot} from 'react-dom/client';
import './style.css';
import AdcsTraffic from './AdcsTraffic.jsx';

async function api(path,data){
 const r=await fetch(path,{headers:{'X-OBC-Token':document.querySelector('meta[name=obc-token]').content,...(data?{'Content-Type':'application/json'}:{})},...(data?{method:'POST',body:JSON.stringify(data)}:{})});
 const result=await r.json();if(!r.ok)throw Error(result.error||JSON.stringify(result.detail)||r.statusText);return result;
}
function NumberControl({id,label,value,unit,min,max,step=0.001,apply,measured}){
 const [draft,setDraft]=useState(String(value)),[dirty,setDirty]=useState(false),[pending,setPending]=useState(false);
 useEffect(()=>{if(!dirty&&!pending)setDraft(String(value))},[value,dirty,pending]);
 return <form className="row" onSubmit={async e=>{e.preventDefault();setPending(true);const ok=await apply(Number(draft));if(ok)setDirty(false);setPending(false)}}><label htmlFor={id}>{label}{measured!==undefined&&<small className="measurement">韌體最近讀回：{measured.toFixed(4)} {unit}</small>}</label><div className="value"><input id={id} type="number" min={min} max={max} step={step} value={draft} required onChange={e=>{setDirty(true);setDraft(e.target.value)}}/><span className="unit">{unit}</span><button disabled={pending}>套用</button></div></form>;
}
function Toggle({name,label,enabled,control}){
 const [pending,setPending]=useState(false),[requested,setRequested]=useState(enabled);
 return <label className="row">{label}<input type="checkbox" checked={pending?requested:enabled} disabled={pending} onChange={async e=>{const next=e.target.checked;setRequested(next);setPending(true);await control({action:'fault',name,enabled:next});setPending(false)}}/></label>;
}
function App(){
 const [state,setState]=useState(null),[notice,setNotice]=useState(''),[error,setError]=useState(false),[output,setOutput]=useState(''),[command,setCommand]=useState(''),[busy,setBusy]=useState(false),[readings,setReadings]=useState({}),[adcsPackets,setAdcsPackets]=useState([]);
 const cursor=useRef(0),adcsCursor=useRef(0),lines=useRef(['']),terminal=useRef(),input=useRef(),history=useRef([]),historyIndex=useRef(0),sending=useRef(false);
 useEffect(()=>{let closed=false,timer;async function poll(){try{const s=await api('/api/state?cursor='+cursor.current+'&adcs_cursor='+adcsCursor.current);if(closed)return;setState(s);if(s.adcs){setAdcsPackets(previous=>[...previous,...s.adcs.requests].slice(-200));adcsCursor.current=s.adcs.cursor;}for(const c of s.output){if(c==='\n')lines.current.push('');else if(c==='\b')lines.current[lines.current.length-1]=lines.current.at(-1).slice(0,-1);else if(c==='\t'||c>=' ')lines.current[lines.current.length-1]+=c;}if(lines.current.length>3000)lines.current=lines.current.slice(-3000);setOutput(lines.current.join('\n'));cursor.current=s.cursor;}catch(e){if(!closed){setError(true);setNotice(e.message)}}if(!closed)timer=setTimeout(poll,400)}poll();return()=>{closed=true;clearTimeout(timer)}},[]);
 useEffect(()=>{if(terminal.current)terminal.current.scrollTop=terminal.current.scrollHeight},[output]);
 async function control(data){try{const r=await api('/api/control',data);setError(false);
 setState(previous=>{if(!previous)return previous;if(data.action==='input')return {...previous,inputs:previous.inputs.map(x=>x.name===data.name?{...x,value:r.requested}:x)};if(data.action==='battery')return {...previous,battery:data.value};if(data.action==='shift')return {...previous,shift:data.value};if(data.action==='fault')return {...previous,faults:data.enabled?[...new Set([...previous.faults,data.name])]:previous.faults.filter(x=>x!==data.name)};return previous});
 if(data.action==='command'&&data.line==='adc print'&&r.output){const measured={};for(const m of r.output.matchAll(/^\s*(\d+)\s+([-+\d.]+)\s+([-+\d.]+)\s+([-+\d.]+)/gm))measured[Number(m[1])]=Number(m[2]);setReadings(measured)}
 setNotice(r.raw!==undefined?`已送入 ADC ${r.name}：${r.requested} ${r.unit}${r.clipped?'（ADC 已飽和）':''}；等待韌體下一次取樣`:(data.action==='command'?'':'已套用'));return true}catch(e){setError(true);setNotice(e.message);return false}}
 async function send(line=command){if(sending.current||!line.trim())return;sending.current=true;setBusy(true);setNotice('等待韌體回應…');history.current.push(line);historyIndex.current=history.current.length;setCommand('');await control({action:'command',line});sending.current=false;setBusy(false);input.current.focus()}
 function recall(delta){historyIndex.current=Math.max(0,Math.min(history.current.length,historyIndex.current+delta));setCommand(history.current[historyIndex.current]||'')}
 const fault=name=>state?.faults.includes(name)||false;
 return <><header><h1>OBC100</h1></header><main><div className="parameters">
 <section className="adc-panel"><h2>ADC</h2><div className="controls adc-controls">{state?.inputs.map(item=><NumberControl key={item.name} id={item.name} label={item.name} value={item.value} measured={readings[item.firmware_index]} unit={item.unit} min={item.kind==='temperature'?undefined:0} step={item.kind==='temperature'?0.1:0.001} apply={value=>control({action:'input',name:item.name,value})}/>)}</div></section>
 <section><h2>EPS</h2><div className="controls"><NumberControl id="battery" label="電池組電壓" value={state?.battery??16} unit="V" min={0} max={65.535} apply={value=>control({action:'battery',value})}/><Toggle name="eps_disconnect" label="EPS 斷線" enabled={fault('eps_disconnect')} control={control}/><Toggle name="eps_checksum" label="Checksum 錯誤" enabled={fault('eps_checksum')} control={control}/></div></section>
 <section><h2>RTC</h2><div className="controls"><NumberControl id="shift" label="時間偏移" value={state?.shift??0} unit="秒" min={-315360000} max={315360000} step={1} apply={value=>control({action:'shift',value})}/><Toggle name="rtc_freeze" label="停止時鐘" enabled={fault('rtc_freeze')} control={control}/></div></section></div>
 <div className="console"><section><div className="console-head"><h2>UART console</h2></div><div className="terminal"><pre id="output" ref={terminal} tabIndex="0" aria-label="UART 即時輸出">{output}</pre><form id="command-form" onSubmit={e=>{e.preventDefault();send()}}><span aria-hidden="true">&gt;</span><input id="command" ref={input} aria-label="UART command" autoComplete="off" maxLength="128" value={command} onChange={e=>setCommand(e.target.value)} onKeyDown={e=>{if(e.key==='ArrowUp'||e.key==='ArrowDown'){e.preventDefault();recall(e.key==='ArrowUp'?-1:1)}}}/><button disabled={busy}>送出</button></form></div><div className="shortcuts">{[['ADC 量測','adc print'],['EPS 遙測','EPS_ANALOG_HK_DATA'],['EPS 設定','EPS_CONFIG'],['RTC 時間','RTC_DATE']].map(([label,cmd])=><button key={cmd} disabled={busy} onClick={()=>send(cmd)}>{label}</button>)}{['ADCS_GET_TIME','ADCS_GET_Q'].map(cmd=><button key={cmd} disabled={busy||!state?.adcs?.connected} onClick={()=>send(cmd)}>{cmd}</button>)}<button onClick={()=>{lines.current=[''];setOutput('')}}>清除畫面</button></div><div id="message" className={error?'error':''} role="status" aria-live="polite">{notice}</div></section><AdcsTraffic state={state?.adcs} packets={adcsPackets}/></div></main></>;
}
createRoot(document.getElementById('app')).render(<App/>);
