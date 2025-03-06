#[cxx::bridge(namespace = "edgeworker::rust::rjs")]
mod ffi {
    extern "Rust" {}
}

pub extern "C" fn init_rjs(isolate: *mut v8::Isolate, context: *mut v8::Context) {
    todo!("register");
}

fn main() {
    // Initialize V8.
    let platform = v8::new_default_platform(0, false).make_shared();
    v8::V8::initialize_platform(platform);
    v8::V8::initialize();

    {
        // Create a new Isolate and make it the current one.
        let isolate = &mut v8::Isolate::new(v8::CreateParams::default());

        // Create a stack-allocated handle scope.
        let handle_scope = &mut v8::HandleScope::new(isolate);

        // Create a new context.
        let context = v8::Context::new(handle_scope, Default::default());

        // Enter the context for compiling and running the hello world script.
        let scope = &mut v8::ContextScope::new(handle_scope, context);

        // Create a string containing the JavaScript source code.
        let code = v8::String::new(scope, "'Hello' + ' World!'").unwrap();

        // Compile the source code.
        let script = v8::Script::compile(scope, code, None).unwrap();
        // Run the script to get the result.
        let result = script.run(scope).unwrap();

        // Convert the result to a string and print it.
        let result = result.to_string(scope).unwrap();
        println!("{}", result.to_rust_string_lossy(scope));

        // Use the JavaScript API to generate a WebAssembly module.
        //
        // |bytes| contains the binary format for the following module:
        //
        //     (func (export "add") (param i32 i32) (result i32)
        //       get_local 0
        //       get_local 1
        //       i32.add)
        //
        let c_source = r#"
            let bytes = new Uint8Array([
              0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01,
              0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x07,
              0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a, 0x09, 0x01,
              0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b
            ]);
            let module = new WebAssembly.Module(bytes);
            let instance = new WebAssembly.Instance(module);
            instance.exports.add(3, 4);
          "#;
        // Create a string containing the JavaScript source code.
        let source = v8::String::new(scope, c_source).unwrap();

        // Compile the source code.
        let script = v8::Script::compile(scope, source, None).unwrap();

        // Run the script to get the result.
        let result = script.run(scope).unwrap();

        // Print the result.
        let result = result.to_uint32(scope).unwrap();
        println!("3 + 4 = {}", result.value());
    }

    unsafe {
        v8::V8::dispose();
    }
    v8::V8::dispose_platform();
}
