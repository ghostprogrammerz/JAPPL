const fs = require('fs');
const Packager = require('@turbowarp/packager');

async function main() {
  const input = process.argv[2];
  const output = process.argv[3];
  if (!input || !output) throw new Error('usage: node package-runtime.js input.sb3 output.html');
  const projectData = fs.readFileSync(input);
  const loadedProject = await Packager.loadProject(projectData);
  const packager = new Packager.Packager();
  packager.project = loadedProject;
  const result = await packager.package();
  if (result.type !== 'text/html') throw new Error(`Packager returned ${result.type}`);
  fs.writeFileSync(output, Buffer.from(result.data));
}
main().catch(error => { console.error(error.stack || error); process.exit(1); });
