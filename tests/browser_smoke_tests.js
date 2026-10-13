// SPDX-License-Identifier: GPL-2.0-only
const assert = require('node:assert/strict');
const {chromium} = require('playwright');

(async () => {
  const browser = await chromium.launch({args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader']});
  try {
    const page = await browser.newPage({viewport: {width: 1280, height: 720}});
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    page.on('console', message => {if (message.type() === 'error') errors.push(message.text());});
    await page.goto(process.argv[2] || 'http://127.0.0.1:8000/pafta_web.html');
    try {
      await page.waitForFunction(() => document.getElementById('loading').hidden, null, {timeout: 90000});
    } catch (error) {
      await page.screenshot({path: process.argv[3] || 'build/web/browser-smoke.png'});
      throw new Error(`${error.message}: ${await page.locator('#loading').innerText()}; ${errors.join('; ')}`);
    }
    const canvas = page.locator('#canvas');
    await canvas.click();
    assert(await canvas.evaluate(element => document.activeElement === element), 'canvas did not receive focus');
    await page.evaluate(() => new Promise(resolve => {
      let frames = 0;
      function tick() {if (++frames === 10) resolve(); else requestAnimationFrame(tick);}
      requestAnimationFrame(tick);
    }));
    assert(await page.evaluate(() => {
      const gl = document.getElementById('canvas').getContext('webgl2');
      return gl && !gl.isContextLost() && gl.drawingBufferWidth > 0 && gl.drawingBufferHeight > 0;
    }), 'compiled editor did not keep a live WebGL2 context');
    await page.screenshot({path: process.argv[3] || 'build/web/browser-smoke.png'});
    assert.deepEqual(errors, [], 'browser reported runtime errors');
    console.log('Compiled WASM startup, WebGL2, canvas focus, and frame-loop smoke check passed.');
  } finally {
    await browser.close();
  }
})().catch(error => {console.error(error); process.exitCode = 1;});
