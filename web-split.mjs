/* Build web_dist Hermes TAHAP TERPISAH (workaround, device 2026-10-09).
 *
 * web.mjs resmi (typecheck + vite dalam SATU proses) segfault (SIGSEGV
 * si_addr=NULL) di device ini — terisolasi lewat 12+ eksperimen A/B:
 * typecheck saja OK, vite saja OK (bahkan dgn public/icons asli), tapi
 * kombinasi keduanya dalam satu proses mati di titik acak.
 *
 * Pakai: node web-split.mjs typecheck   (proses 1, harus status 0)
 *        node web-split.mjs vite        (proses 2, build + publish + record)
 */
import path from 'node:path';
import { cpSync, existsSync, mkdirSync, renameSync, rmSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const source = '/root/.hermes/hermes-agent';
const root = path.join(source, 'web');
const finalOut = path.join(source, 'hermes_cli/web_dist');
const stage = '/tmp/hermes-web-stage';
const phase = process.argv[2];
if (phase !== 'typecheck' && phase !== 'vite') throw new Error('pakai: typecheck|vite');

const { workspaceTool } = await import(
  '/root/.hermes/hermes-agent/scripts/build/frontend-common.mjs');
const { buildInputs, recordProduct } = await import(
  '/root/.hermes/hermes-agent/scripts/build/freshness.mjs');

if (phase === 'typecheck') {
  const tsModule = await import(pathToFileURL(workspaceTool(source, 'web', 'typescript')).href);
  const ts = tsModule.default ?? tsModule;
  const diagnostics = [];
  const host = ts.createSolutionBuilderHost(ts.sys, undefined, d => diagnostics.push(d));
  mkdirSync('/tmp/hermes-web-tscratch', { recursive: true });
  const buildInfo = new Map();
  host.writeFile = (file, data) => {
    if (!file.endsWith('.tsbuildinfo')) throw new Error(`Unexpected TypeScript emit: ${file}`);
    if (!buildInfo.has(file)) buildInfo.set(file, path.join('/tmp/hermes-web-tscratch', `ts-${buildInfo.size}.tsbuildinfo`));
    ts.sys.writeFile(buildInfo.get(file), data);
  };
  const status = ts.createSolutionBuilder(host, [path.join(root, 'tsconfig.json')], { force: true }).build();
  console.log(`typecheck status=${status} diagnostics=${diagnostics.length}`);
  if (status !== 0 || diagnostics.length) throw new Error('typecheck gagal');
  console.log('TYPECHECK OK');
} else {
  const inputs = buildInputs(source, 'web', { icons: source + '/web/public' });
  console.log('inputs ok');
  const { build } = await import(pathToFileURL(workspaceTool(source, 'web', 'vite')).href);
  console.log('vite import ok');
  rmSync(stage, { recursive: true, force: true });
  mkdirSync(stage, { recursive: true });
  const publicDir = path.join(stage, 'pub');
  mkdirSync(publicDir);
  if (existsSync(path.join(root, 'public'))) cpSync(path.join(root, 'public'), publicDir, { recursive: true });
  cpSync(source + '/web/public', publicDir, { recursive: true });
  const product = path.join(stage, 'product');
  mkdirSync(product);
  await build({ root, configFile: path.join(root, 'vite.config.ts'), configLoader: 'runner',
    cacheDir: path.join(stage, 'vite-cache'), publicDir, logLevel: 'info',
    build: { outDir: product, emptyOutDir: true } });
  console.log('vite build ok, publish...');
  rmSync(finalOut, { recursive: true, force: true });
  mkdirSync(path.dirname(finalOut), { recursive: true });
  renameSync(product, finalOut);
  recordProduct({ source, product: 'web', out: finalOut, inputs });
  console.log('VITE+PUBLISH+RECORD OK:', path.join(finalOut, 'index.html'));
}
