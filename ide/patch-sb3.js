const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const JSZip = require('jszip');

function extFor(c) {
  const mime = (c.type || '').toLowerCase();
  if (mime === 'image/svg+xml') return 'svg';
  if (mime === 'image/jpeg' || mime === 'image/jpg') return 'jpg';
  if (mime === 'image/gif') return 'gif';
  if (mime === 'image/webp') return 'webp';
  const m = String(c.filename || '').toLowerCase().match(/\.([a-z0-9]+)$/);
  return m ? m[1] : 'png';
}

(async () => {
  const sb3Path = process.argv[2];
  const metaPath = process.argv[3];
  if (!sb3Path || !metaPath) throw new Error('usage: patch-sb3.js project.sb3 costumes.json');

  const metadata = JSON.parse(fs.readFileSync(metaPath, 'utf8'));
  const zip = await JSZip.loadAsync(fs.readFileSync(sb3Path));
  const project = JSON.parse(await zip.file('project.json').async('string'));
  const bySprite = new Map(metadata.map(x => [String(x.sprite), x.costumes || []]));

  for (const target of project.targets || []) {
    const costumes = bySprite.get(String(target.name));
    if (!costumes || costumes.length === 0) continue;

    target.costumes = [];
    target.currentCostume = 0;

    for (const c of costumes) {
      if (!c.dataUrl || !c.dataUrl.includes(',')) continue;
      const b64 = c.dataUrl.slice(c.dataUrl.indexOf(',') + 1);
      const data = Buffer.from(b64, 'base64');
      if (!data.length) continue;

      const assetId = crypto.createHash('md5').update(data).digest('hex');
      const dataFormat = extFor(c);
      const md5ext = `${assetId}.${dataFormat}`;
      const width = Number(c.width) || 0;
      const height = Number(c.height) || 0;

      target.costumes.push({
        assetId,
        name: c.name || path.basename(c.filename || 'Costume', path.extname(c.filename || '')),
        bitmapResolution: 1,
        dataFormat,
        md5ext,
        rotationCenterX: width ? width / 2 : 0,
        rotationCenterY: height ? height / 2 : 0
      });
      zip.file(md5ext, data);
    }

    if (target.costumes.length) zip.remove('bcf454acf82e4f0b1f90e8b3d7c16c7d.png');
  }

  zip.file('project.json', JSON.stringify(project));
  fs.writeFileSync(sb3Path, await zip.generateAsync({ type: 'nodebuffer', compression: 'STORE' }));
})().catch(err => { console.error(err.stack || err); process.exit(1); });
