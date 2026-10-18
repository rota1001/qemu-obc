/** Real browser clicks -> FastAPI -> actual guest commands. No API-only mocks.
 * PYTHONPATH=. OBC_WEB_PYTHON=.venv-web/bin/python node tests/browser_apply.mjs
 */
import {chromium} from '../web/node_modules/playwright-core/index.mjs';
import {spawn} from 'node:child_process';
import {createInterface} from 'node:readline';
import {mkdir,writeFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
const cwd=fileURLToPath(new URL('../',import.meta.url));
const child=spawn(process.env.OBC_WEB_PYTHON||'.venv-web/bin/python',
 ['-u','examples/dashboard.py','--web','--firmware',
  process.env.OBC_FIRMWARE||fileURLToPath(new URL('../../../../obc/build/TMS570LC43_OBC_PLUS_20240102.out',import.meta.url))],
 {cwd,env:{...process.env,PYTHONPATH:cwd},stdio:['ignore','pipe','pipe']});
let browser;const evidence=[];let errors=[];
const artifacts=fileURLToPath(new URL('artifacts/',import.meta.url));
const serverLog=[];child.stderr.on('data',x=>serverLog.push(x.toString()));
try{
 const url=await new Promise((resolve,reject)=>{const lines=createInterface({input:child.stdout});const timer=setTimeout(()=>reject(Error('Dashboard startup timeout: '+serverLog.join(''))),15000);lines.on('line',s=>{const m=s.match(/Dashboard: (http:\/\/[^ ]+)/);if(m){clearTimeout(timer);resolve(m[1])}});child.on('exit',c=>{clearTimeout(timer);reject(Error('Dashboard exited: '+c+serverLog.join('')))});});
 browser=await chromium.launch({executablePath:process.env.CHROME_BIN||'/usr/bin/google-chrome',headless:true,args:['--no-sandbox']});
 const page=await browser.newPage({viewport:{width:1400,height:950}});
 page.on('pageerror',e=>errors.push(e.message));
 await page.goto(url);await page.locator('#obc_board').waitFor();
 async function apply(id,value){const input=page.locator('#'+id);await input.fill(String(value));const response=page.waitForResponse(r=>r.url().endsWith('/api/control')&&r.request().postDataJSON().action!=='command');await input.locator('..').getByRole('button',{name:'套用'}).click();const r=await response;assert.equal(r.status(),200,await r.text());await input.locator('..').getByRole('button').waitFor({state:'visible'});return r.json();}
 async function cmd(line){await page.locator('#command').fill(line);const response=page.waitForResponse(r=>r.url().endsWith('/api/control')&&r.request().postDataJSON().action==='command');await page.getByRole('button',{name:'送出',exact:true}).click();const r=await response;assert.equal(r.status(),200,await r.text());await page.waitForFunction(()=>!document.querySelector('#command-form button').disabled);return (await r.json()).output;}
 async function toggle(label,enabled){const box=page.getByText(label,{exact:true}).locator('input');const response=page.waitForResponse(r=>r.url().endsWith('/api/control')&&r.request().postDataJSON().action==='fault');await box.setChecked(enabled);const r=await response;assert.equal(r.status(),200,await r.text());await box.waitFor({state:'visible'});await page.waitForFunction(label=>{const row=[...document.querySelectorAll('label.row')].find(x=>x.textContent===label);return !row.querySelector('input').disabled},label);}
 const cases=[['obc_board',0,45,10,.15],['obc_1v2',1,1,1.2,.003],['obc_3v3_sys',2,3,3.2,.003],['obc_3v3_adc',3,3.1,3.3,.003],['obc_ext_3v3_in',4,2.9,3.1,.003],['obc_ext_3v3_out',5,2.8,0,.003],['obc_5v_current',7,.2,.1,.004],['obc_ext_5v_current',8,.3,.05,.004],['obc_5v_in',9,4.8,5,.004],['obc_5v_out',10,4.6,4.9,.004]];
 for(const phase of [2,3]){
  for(const c of cases)await apply(c[0],c[phase]);
  const deadline=Date.now()+35000;let rows;
  while(true){const output=await cmd('adc print');rows=new Map([...output.matchAll(/^\s*(\d+)\s+([-+\d.]+)\s+([-+\d.]+)\s+([-+\d.]+)/gm)].map(m=>[Number(m[1]),m.slice(2).map(Number)]));if(cases.every(c=>rows.get(c[1])?.every(x=>Math.abs(x-c[phase])<=c[4])))break;assert.ok(Date.now()<deadline,'Firmware readings did not settle: '+output);await page.waitForTimeout(500)}
  for(const c of cases){evidence.push({parameter:c[0],requested:c[phase],os:rows.get(c[1]),passed:true});const label=page.locator('label[for="'+c[0]+'"]');await label.getByText(/韌體最近讀回/).waitFor();}
 }
 for(const v of [12,16.123]){await apply('battery',v);const out=await cmd('EPS_ANALOG_HK_DATA');const hex=[...out.matchAll(/address\[[0-9a-f]+\] : \[([0-9a-f]{8})\]/g)].map(m=>m[1]).join('');const bytes=Buffer.from(hex,'hex');const packs=[90,110,130].map(i=>bytes.readUInt16BE(i));assert.deepEqual(packs,[Math.round(v*1000),Math.round(v*1000),Math.round(v*1000)]);evidence.push({parameter:'battery',requested:v,os:packs,passed:true})}
 await toggle('EPS 斷線',true);assert.match(await cmd('EPS_ANALOG_HK_DATA'),/i2c=-3/);await toggle('EPS 斷線',false);assert.match(await cmd('EPS_ANALOG_HK_DATA'),/address\[/);evidence.push({parameter:'eps_disconnect',passed:true});
 await toggle('Checksum 錯誤',true);assert.match(await cmd('EPS_CONFIG'),/checksum mismatch/);await toggle('Checksum 錯誤',false);assert.doesNotMatch(await cmd('EPS_CONFIG'),/checksum mismatch/);evidence.push({parameter:'eps_checksum',passed:true});
 const rtc=async()=>{const text=await cmd('RTC_DATE');return new Date(text.match(/RTC Time: (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)/)[1].replace(' ','T')+'Z').getTime()/1000};
 await apply('shift',0);const base=await rtc();for(const s of [86400,-3600,0]){await apply('shift',s);const observed=await rtc();assert.ok(Math.abs(observed-base-s)<=2);evidence.push({parameter:'shift',requested:s,os:observed-base,passed:true})}
 await toggle('停止時鐘',true);const stopped=await rtc();await page.waitForTimeout(1200);assert.equal(await rtc(),stopped);await apply('shift',86400);assert.equal(await rtc()-stopped,86400);await apply('shift',0);assert.equal(await rtc(),stopped);await toggle('停止時鐘',false);await page.waitForTimeout(1200);assert.ok(await rtc()>stopped);evidence.push({parameter:'rtc_freeze_and_shift',passed:true});
 assert.deepEqual(errors,[],'Browser errors');
 assert.ok(await page.locator('#output').innerText());
 await mkdir(artifacts,{recursive:true});await page.screenshot({path:artifacts+'dashboard-desktop.png',fullPage:true});
 await page.setViewportSize({width:390,height:844});await page.screenshot({path:artifacts+'dashboard-mobile.png',fullPage:true});assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=window.innerWidth));
 console.log(`PASS: ${evidence.length} browser/firmware observations; no page errors`);
}catch(error){console.error('Browser test failed:',error);throw error;}finally{
 await mkdir(artifacts,{recursive:true});await writeFile(artifacts+'browser_results.json',JSON.stringify({evidence,errors},null,2));
 if(browser)await browser.close();child.kill('SIGINT');await new Promise(resolve=>{if(child.exitCode!==null)resolve();else child.once('exit',resolve)});
}
