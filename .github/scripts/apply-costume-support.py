from pathlib import Path
import json

# server.c
p=Path('server.c'); s=p.read_text()
if '#define MAX_REQUEST (4 * 1024 * 1024)' in s:
    s=s.replace('#define MAX_REQUEST (4 * 1024 * 1024)', '#define MAX_REQUEST (32 * 1024 * 1024)')
old='''static void handle_compile(int fd, const char *src, size_t src_len) {
    (void)src_len;
    char tmp[128];'''
new='''static void handle_compile(int fd, const char *src, size_t src_len) {
    /* Optional IDE costume metadata follows this delimiter. It is deliberately
       kept outside JAPPL source so the normal parser remains unchanged. */
    static const char marker[] = "\\n--JAPPL-COSTUMES--\\n";
    const char *meta = strstr(src, marker);
    size_t source_len = meta ? (size_t)(meta - src) : src_len;
    char *source = malloc(source_len + 1);
    if (!source) {
        http_respond(fd, 500, "text/plain", "Out of memory\\n", 14);
        return;
    }
    memcpy(source, src, source_len);
    source[source_len] = '\\0';

    char meta_path[128] = {0};
    if (meta) {
        snprintf(meta_path, sizeof(meta_path), TMPDIR "/jappl_%d_costumes.json", (int)getpid());
        FILE *mf = fopen(meta_path, "wb");
        size_t meta_len = src_len - source_len - strlen(marker);
        if (!mf || fwrite(meta + strlen(marker), 1, meta_len, mf) != meta_len) {
            if (mf) fclose(mf);
            free(source);
            unlink(meta_path);
            http_respond(fd, 400, "text/plain", "Invalid costume metadata\\n", 26);
            return;
        }
        fclose(mf);
    }

    char tmp[128];'''
if old not in s: raise SystemExit('server compile start not found')
s=s.replace(old,new,1)
old='''    parser_init(&parser, src);
    Program *program = parser_parse(&parser);'''
new='''    parser_init(&parser, source);
    Program *program = parser_parse(&parser);'''
if old not in s: raise SystemExit('parser call not found')
s=s.replace(old,new,1)
old='''    if (parser.errors > 0) {
        http_respond(fd, 400, "text/plain",
                     "Compilation failed — check server stderr for details.\\n", 56);
        return;
    }'''
new='''    if (parser.errors > 0) {
        free(source);
        unlink(meta_path);
        http_respond(fd, 400, "text/plain",
                     "Compilation failed — check server stderr for details.\\n", 56);
        return;
    }'''
if old not in s: raise SystemExit('parser error block not found')
s=s.replace(old,new,1)
old='''    if (emit_sb3(program, tmp) != 0) {
        http_respond(fd, 500, "text/plain", "Emitter failed to write .sb3\\n", 31);
        return;
    }

    size_t len = 0;'''
new='''    if (emit_sb3(program, tmp) != 0) {
        free(source);
        unlink(meta_path);
        http_respond(fd, 500, "text/plain", "Emitter failed to write .sb3\\n", 31);
        return;
    }

    if (meta_path[0]) {
        char command[1024];
        snprintf(command, sizeof(command),
                 "node \\\"%s/patch-sb3.js\\\" \\\"%s\\\" \\\"%s\\\"",
                 g_ide_dir, tmp, meta_path);
        if (system(command) != 0) {
            free(source);
            unlink(meta_path);
            unlink(tmp);
            http_respond(fd, 400, "text/plain", "Failed to apply costume assets\\n", 34);
            return;
        }
        unlink(meta_path);
    }
    free(source);

    size_t len = 0;'''
if old not in s: raise SystemExit('emitter block not found')
s=s.replace(old,new,1)
p.write_text(s)

# IDE compile request
p=Path('ide/index.html'); s=p.read_text()
old='''  const src = buildJapplSource();
  const backendUrl = document.getElementById('backend-url').value.trim();

  try {
    const res = await fetch(`${backendUrl}/compile`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: src,
    });'''
new='''  const src = buildJapplSource();
  const backendUrl = document.getElementById('backend-url').value.trim();

  /* Costume files are IDE assets rather than JAPPL source. Send their data
     to the compiler in an optional metadata section. */
  const costumeMeta = sprites.map(sp => ({
    sprite: sp.name,
    costumes: (sp.costumes || []).map(c => ({
      name: c.name,
      dataUrl: c.dataUrl,
      type: c.type || '',
      filename: c.filename || '',
      width: c.width || 0,
      height: c.height || 0
    }))
  })).filter(x => x.costumes.length);
  const requestBody = costumeMeta.length
    ? src + '\\n--JAPPL-COSTUMES--\\n' + JSON.stringify(costumeMeta)
    : src;

  try {
    const res = await fetch(`${backendUrl}/compile`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain; charset=utf-8' },
      body: requestBody,
    });'''
if old not in s: raise SystemExit('IDE compile block not found')
s=s.replace(old,new,1)
# Dimension-aware costume loader.
old='''      sp.costumes.push({
        name: file.name.replace(/\\.[^.]+$/, ''),
        dataUrl: e.target.result,
        type: file.type,
        filename: file.name,
      });
      pending--;
      if (pending === 0) renderCostumeList(sp);'''
new='''      const costume = {
        name: file.name.replace(/\\.[^.]+$/, ''),
        dataUrl: e.target.result,
        type: file.type,
        filename: file.name,
        width: 0,
        height: 0,
      };
      const img = new Image();
      img.onload = () => {
        costume.width = img.naturalWidth || img.width || 0;
        costume.height = img.naturalHeight || img.height || 0;
        sp.costumes.push(costume);
        pending--;
        if (pending === 0) renderCostumeList(sp);
      };
      img.onerror = () => {
        sp.costumes.push(costume);
        pending--;
        if (pending === 0) renderCostumeList(sp);
      };
      img.src = costume.dataUrl;'''
if old not in s: raise SystemExit('costume loader block not found')
s=s.replace(old,new,1)
p.write_text(s)

# package dependency
p=Path('ide/package.json'); j=json.loads(p.read_text()); j.setdefault('dependencies',{})['jszip']='^3.10.1'; p.write_text(json.dumps(j,indent=2)+'\n')
