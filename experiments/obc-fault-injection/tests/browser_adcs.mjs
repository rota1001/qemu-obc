/** Real React console -> OBC SCI3 -> passive Python peer -> Hex display. */
import {chromium} from '../web/node_modules/playwright-core/index.mjs';
import {spawn} from 'node:child_process';
import {createInterface} from 'node:readline';
import {mkdir,writeFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
const cwd=fileURLToPath(new URL('../',import.meta.url));
const firmware=process.env.OBC_FIRMWARE||fileURLToPath(new URL('../../../../obc/build/TMS570LC43_OBC_PLUS_20240102.out',import.meta.url));
const child=spawn(process.env.OBC_WEB_PYTHON||'.venv-web/bin/python',
 ['-u','examples/dashboard.py','--web','--firmware',firmware],
 {cwd,env:{...process.env,PYTHONPATH:cwd},stdio:['ignore','pipe','pipe']});
let browser;let stderr='';const errors=[],evidence=[];
child.stderr.on('data',x=>stderr+=x.toString());
const artifacts=fileURLToPath(new URL('artifacts/',import.meta.url));
try{
 const url=await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('Startup timeout: '+stderr)),15000);createInterface({input:child.stdout}).on('line',s=>{const m=s.match(/Dashboard: (http:\/\/[^ ]+)/);if(m){clearTimeout(timer);resolve(m[1])}});child.on('exit',code=>{clearTimeout(timer);reject(Error('Exit '+code+': '+stderr))})});
 browser=await chromium.launch({executablePath:process.env.CHROME_BIN||'/usr/bin/google-chrome',headless:true,args:['--no-sandbox']});
 const page=await browser.newPage({viewport:{width:1400,height:1000}});
 page.on('pageerror',e=>errors.push(e.message));
 await page.goto(url);await page.waitForFunction(()=>{const button=[...document.querySelectorAll('.shortcuts button')].find(b=>b.textContent==='ADCS_GET_TIME');return button&&!button.disabled});
 for(const [command,hex] of [['ADCS_GET_TIME','C9 05 01 00 31'],['ADCS_GET_Q','C9 84 04 00 AF'],['ADCS_SET_TIME 00563412','C8 05 01 00 00 56 34 12 96']]){
  await page.locator('#command').fill(command);
  const responsePromise=page.waitForResponse(r=>r.url().endsWith('/api/control')&&r.request().postDataJSON().action==='command');
  await page.getByRole('button',{name:'送出',exact:true}).click();
  const response=await responsePromise;assert.equal(response.status(),200);
  const output=(await response.json()).output;
  assert.ok(output.includes(command.startsWith('ADCS_GET')?'failed':'success'),output);
  const packet=page.locator('.packet').filter({hasText:hex});
  await packet.waitFor();assert.equal(await packet.locator('code').innerText(),hex);
  assert.match(await packet.innerText(),/未回覆/);
  evidence.push({command,hex,output,response:null,passed:true});
  await page.waitForFunction(()=>!document.querySelector('#command-form button').disabled);
 }
 await page.getByRole('button',{name:'ADCS_GET_TIME',exact:true}).click();
 await page.waitForFunction(()=>document.querySelectorAll('.packet').length===4);
 assert.equal(await page.locator('.packet').count(),4); // no duplicates from polling
 assert.deepEqual(errors,[]);
 await mkdir(artifacts,{recursive:true});
 await page.locator('.adcs-traffic').screenshot({path:artifacts+'adcs-hex-desktop.png'});
 await page.setViewportSize({width:390,height:844});
 await page.locator('.adcs-traffic').screenshot({path:artifacts+'adcs-hex-mobile.png'});
 assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
 console.log('PASS: 4 actual SCI3 requests shown as Hex; no automatic response; no page errors');
}finally{
 await mkdir(artifacts,{recursive:true});await writeFile(artifacts+'adcs_browser.json',JSON.stringify({evidence,errors},null,2));
 if(browser)await browser.close();child.kill('SIGINT');await new Promise(resolve=>{if(child.exitCode!==null)resolve();else child.once('exit',resolve)});
}
