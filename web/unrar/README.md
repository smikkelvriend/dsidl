# Rar uitpakken op de telefoon

`unrar.js` + `unrar.wasm` komen uit [node-unrar-js](https://github.com/YuJianrong/node-unrar.js)
2.0.2 (MIT): de officiële unRAR-broncode (unRAR-licentie van Alexander Roshal,
alleen voor uitpakken) gecompileerd naar WebAssembly. `unrar.js` is gebundeld met

    echo "export { createExtractorFromData } from 'node-unrar-js/esm/index.esm.js';" > entry.js
    npx esbuild entry.js --bundle --format=esm --platform=browser --minify --outfile=unrar.js

De DSi serveert beide bestanden zelf (`/unrar.js`, `/unrar.wasm`), zodat de telefoon
geen internet nodig heeft.
