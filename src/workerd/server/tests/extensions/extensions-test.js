import * as assert from 'node:assert'
import { openDoor } from 'test:module'

// prettier-ignore
const diff = (() => {const richTypes={Date:!0,RegExp:!0,String:!0,Number:!0};return function diff(e,t,r={cyclesFix:!0},a=[]){let c=[];var s=Array.isArray(e);for(const y in e){var p=e[y];const l=s?+y:y;if(y in t){var i=t[y],o="object"==typeof p&&"object"==typeof i;if(!(p&&i&&o)||richTypes[Object.getPrototypeOf(p)?.constructor?.name]||r.cyclesFix&&a.includes(p))p===i||o&&(isNaN(p)?p+""==i+"":+p==+i)||c.push({path:[l],type:"CHANGE",value:i,oldValue:p});else{const u=diff(p,i,r,r.cyclesFix?a.concat([p]):[]);c.push.apply(c,u.map(e=>(e.path.unshift(l),e)))}}else c.push({type:"REMOVE",path:[l],oldValue:e[y]})}var n=Array.isArray(t);for(const f in t)f in e||c.push({type:"CREATE",path:[n?+f:f],value:t[f]});return c}})();

export const test_module_api = {
  test() {
    assert.throws(() => openDoor('test key'))
    assert.equal(openDoor('0p3n s3sam3'), true)
  },
}

export const test_builtin_dynamic_import = {
  async test() {
    await assert.doesNotReject(import('test:module'))
  },
}

// internal modules can't be imported
export const test_builtin_internal_dynamic_import = {
  async test() {
    await assert.rejects(import('test-internal:internal-module'))
  },
}

export const test_wrapped_binding = {
  async test(ctr, env) {
    assert.ok(env.door, 'binding is not present')
    assert.equal(typeof env.door, 'object')
    assert.ok(env.door.tryOpen)
    assert.equal(typeof env.door.tryOpen, 'function')

    // binding uses a different secret specified in the config
    assert.ok(env.door.tryOpen('open sesame'))
    assert.ok(!env.door.tryOpen('bad secret'))

    // check there are no other properties available
    assert.deepEqual(Object.keys(env.door), ['tryOpen'])
    assert.deepEqual(Object.getOwnPropertyNames(env.door), ['tryOpen'])

    assert.ok(env.customDoor, 'custom binding is not present')
    assert.ok(!env.customDoor.tryOpen('open sesame'))
    assert.ok(env.customDoor.tryOpen('custom open sesame'))

    console.log(Object.keys(env.d1))
    console.log(env.d1.prepare)

    // NOTE: you need to be running `npx wrangler dev echoback.js` in another terminal
    // for this to pass (for now)...
    assert.deepStrictEqual(await env.d1.prepare(`select 1`).all(), {
      lol: 'boats',
    })
  },
}
