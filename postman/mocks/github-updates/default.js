// Local read-only GitHub Releases subset. Fixtures contain no project/user data.
const http = require('http');
const crypto = require('crypto');
const version = '2026.1.0-alpha.10';
const name = `3d-bending-simulator-${version}-windows-x86_64.zip`;
const prefix = '/DenisPapushaJava/3d-bending-simulator/releases/download/v' + version + '/';
const bytes = Buffer.from('portable test archive');
const hash = crypto.createHash('sha256').update(bytes).digest('hex');
http.createServer((req, res) => {
  const path = new URL(req.url, 'http://localhost').pathname;
  if (req.method !== 'GET') { res.writeHead(501); return res.end('Not modelled'); }
  if (path === '/repos/DenisPapushaJava/3d-bending-simulator/releases') {
    res.writeHead(200, {'Content-Type':'application/json'});
    return res.end(JSON.stringify([{tag_name:'v'+version, draft:false, prerelease:true,
      published_at:'2026-10-28T12:00:00Z', body:'Local update fixture; no real executable.',
      assets:[{name, state:'uploaded',size:bytes.length,digest:'sha256:'+hash,
        browser_download_url:'https://github.com'+prefix+name},
        {name:'SHA256SUMS.txt',state:'uploaded',size:150,
         browser_download_url:'https://github.com'+prefix+'SHA256SUMS.txt'}]}]));
  }
  if (path === prefix+'SHA256SUMS.txt') { res.writeHead(200, {'Content-Type':'text/plain'}); return res.end(hash+'  '+name+'\n'); }
  if (path === prefix+name) { res.writeHead(200, {'Content-Type':'application/zip','Content-Length':bytes.length}); return res.end(bytes); }
  res.writeHead(501); res.end('Not modelled');
}).listen(process.env.PORT || 4500);
