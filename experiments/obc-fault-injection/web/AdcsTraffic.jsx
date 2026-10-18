import React, {useEffect, useRef} from 'react';

/** Binary SCI3 traffic, captured by the passive Python peer (not console text). */
export default function AdcsTraffic({state, packets}) {
  const log=useRef();
  useEffect(()=>{if(log.current)log.current.scrollTop=log.current.scrollHeight},[packets]);
  return <section className="adcs-traffic" aria-label="ADCS 封包">
    <div className="console-head"><h2>ADCS</h2></div>
    <div className="packet-log" ref={log} tabIndex="0" aria-label="ADCS request records">
      {packets.map(p=><article className="packet" key={p.id} data-packet-id={p.id}>
        <div className="packet-meta"><strong>#{p.id} {p.kind}</strong></div>
        <div className="packet-line"><span>Request</span><code>{p.request}</code></div>
        <div className="packet-line"><span>Response</span><span className="muted">未回覆</span></div>
      </article>)}
    </div>
    {state?.pending_hex&&<div className="packet-pending"><span>接收中（半包）</span><code>{state.pending_hex}</code></div>}
    {state?.error&&<p role="alert">{state.error}</p>}
  </section>;
}
