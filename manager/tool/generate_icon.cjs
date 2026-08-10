const fs = require('fs');
const path = require('path');

const dependencyRoot = process.env.ORGANIZER_NODE_MODULES;
if (!dependencyRoot) throw new Error('ORGANIZER_NODE_MODULES is required');
const sharp = require(path.join(dependencyRoot, 'sharp'));

const project = path.resolve(__dirname, '..');
const source = path.join(project, 'assets', 'Logo_background.svg');
const destination = path.join(project, 'windows', 'runner', 'resources', 'app_icon.ico');

(async () => {
  const png = await sharp(source).resize(256, 256).png().toBuffer();
  const header = Buffer.alloc(22);
  header.writeUInt16LE(0, 0);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(1, 4);
  header.writeUInt8(0, 6);
  header.writeUInt8(0, 7);
  header.writeUInt8(0, 8);
  header.writeUInt8(0, 9);
  header.writeUInt16LE(1, 10);
  header.writeUInt16LE(32, 12);
  header.writeUInt32LE(png.length, 14);
  header.writeUInt32LE(22, 18);
  fs.writeFileSync(destination, Buffer.concat([header, png]));
})();
