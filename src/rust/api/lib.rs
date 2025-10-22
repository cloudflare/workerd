#![feature(must_not_suspend)]
#![warn(must_not_suspend)]

pub mod dns;

pub struct URLSearchParams {
    search: String,
}

impl URLSearchParams {
    pub fn to_string(&self) -> String {
        "<URLSearchParams>".to_string()
    }

    pub fn get_search(&self) -> &str {
        self.search.as_str()
    }

    pub fn set_search(&mut self, search: &str) {
        self.search = search.to_string()
    }
}

// generated code
impl URLSearchParams {
    fn to_string_callback(
        this: *mut Self,
        lock: *mut jsg::ffi::Lock,
        _args: *mut jsg::ffi::Args,
    ) -> jsg::Result<jsg::ffi::Value> {
        let this = unsafe { &mut *this };
        let lock = unsafe { &mut *lock };
        let result = this.to_string();
        let result = jsg::ffi::value_from_string(lock, &result);
        Ok(result)
    }

    fn get_search_callback(
        this: *mut Self,
        lock: *mut jsg::ffi::Lock,
        _args: *mut jsg::ffi::Args,
    ) -> jsg::Result<jsg::ffi::Value> {
        let this = unsafe { &mut *this };
        let lock = unsafe { &mut *lock };
        let result = this.get_search();
        let result = jsg::ffi::value_from_string(lock, result);
        Ok(result)
    }

    fn set_search_callback(
        this: *mut Self,
        lock: *mut jsg::ffi::Lock,
        args: *mut jsg::ffi::Args,
    ) -> jsg::Result<()> {
        let this = unsafe { &mut *this };
        let lock = unsafe { &mut *lock };
        let args = unsafe { &mut *args };
        let arg0 = args.get_arg(0);
        let arg0 = jsg::ffi::string_from_value(lock, arg0);
        this.set_search(arg0);
        Ok(())
    }
}

// generated code
impl jsg::Resource for URLSearchParams {
    fn members() -> Vec<jsg::Member<Self>> {
        vec![
            jsg::Member::Method {
                name: "toString",
                callback: Box::new(URLSearchParams::to_string_callback),
            },
            jsg::Member::Property {
                name: "search",
                getter: Box::new(URLSearchParams::get_search_callback),
                setter: Some(Box::new(URLSearchParams::set_search_callback)),
            },
        ]
    }
}

impl jsg::Type for URLSearchParams {}

pub struct URL {
    search_params: jsg::Ref<URLSearchParams>,
}

impl URL {
    pub fn new(lock: &mut jsg::Lock, _str: &str) -> jsg::Ref<Self> {
        // demonstrate creation of nested resources.
        let search_params = lock.alloc(URLSearchParams {
            search: "".to_string(),
        });
        lock.alloc(URL { search_params })
    }

    pub fn search_params(&self) -> jsg::Ref<URLSearchParams> {
        self.search_params.clone()
    }

    pub fn search<'a, 'b>(&'a self, lock: &'b jsg::Lock) -> String {
        self.search_params.as_ref(lock).to_string()
    }

    pub fn set_search(&mut self, lock: &jsg::Lock, search: &str) {
        self.search_params.as_mut(lock).set_search(search)
    }

    // This async function demonstrates the usage of io context.
    pub async fn new_resolved(lock: jsg::Lock, url: &str) -> jsg::Result<jsg::Ref<URL>> {
        lock.await_io(Self::resolve_url(url), |mut lock: jsg::Lock, _i| {
            let url = URL::new(&mut lock, url);
            Ok(url)
        })
    }

    // Some potentially slow async function that doesn't need v8.
    async fn resolve_url(url: &str) -> jsg::Result<String> {
        std::future::pending::<()>().await;
        Ok(url.to_string())
    }
}

// generated code
impl URL {
    fn search_params_callback(
        _this: *mut Self,
        _lock: *mut jsg::ffi::Lock,
        _args: *mut jsg::ffi::Args,
    ) -> jsg::Result<jsg::ffi::Value> {
        todo!();
    }

    fn get_search_callback(
        _this: *mut Self,
        _lock: *mut jsg::ffi::Lock,
        _args: *mut jsg::ffi::Args,
    ) -> jsg::Result<jsg::ffi::Value> {
        todo!();
    }

    fn set_search_callback(
        _this: *mut Self,
        _lock: *mut jsg::ffi::Lock,
        _args: *mut jsg::ffi::Args,
    ) -> jsg::Result<()> {
        todo!();
    }
}

// generated code
impl jsg::Resource for URL {
    fn members() -> Vec<jsg::Member<Self>> {
        vec![
            jsg::Member::Constructor,
            jsg::Member::Property {
                name: "searchParams",
                getter: Box::new(URL::search_params_callback),
                setter: None,
            },
            jsg::Member::Property {
                name: "search",
                getter: Box::new(URL::get_search_callback),
                setter: Some(Box::new(URL::set_search_callback)),
            },
        ]
    }
}

impl jsg::Type for URL {}

// generated code
pub fn register_types(r: &mut jsg::TypeRegistrar) {
    r.register_resource::<URL>();
    r.register_resource::<URLSearchParams>();
}
