// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Methods take and return typed promises, also in containers, carry Rust values through
// reactions, chain promises, keep a promise and its resolver in fields, and report errors by
// rejecting, without `unsafe`.
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

pub struct Carried {
    pub count: usize,
}

jsg::jsg_traced!(Carried);

#[jsg_resource]
pub struct Waiter {
    promise: jsg::RefCell<Option<jsg::TracedPromise<Carried>>>,
    resolver: jsg::RefCell<Option<jsg::TracedPromiseResolver<Carried>>>,
}

#[jsg_resource]
impl Waiter {
    #[jsg_method]
    fn start(&self, lock: &mut jsg::Lock<'_>) -> Result<(), jsg::Error> {
        let (promise, resolver) = jsg::Promise::<Carried>::new_with_resolver(lock)?;
        *self.promise.borrow_mut(lock) = Some(promise.into_traced(lock));
        *self.resolver.borrow_mut(lock) = Some(resolver.into_traced(lock));
        Ok(())
    }

    #[jsg_method]
    fn finish(&self, lock: &mut jsg::Lock<'_>, count: u32) -> Result<(), jsg::Error> {
        let resolver = self.resolver.borrow_mut(lock).take();
        if let Some(resolver) = resolver {
            resolver.as_local(lock).resolve(
                lock,
                Carried {
                    count: count as usize,
                },
            )?;
        }
        Ok(())
    }

    #[jsg_method]
    fn count<'s>(&self, lock: &mut jsg::Lock<'s>) -> Result<jsg::Promise<'s, u32>, jsg::Error> {
        let promise = self.promise.borrow_mut(lock).take();
        lock.eval_now(|lock| {
            let promise = promise
                .ok_or_else(|| jsg::Error::new_type_error("not started"))?
                .into_local(lock);
            promise.then(lock, (), |_lock, (), carried| Ok(carried.count as u32))
        })
    }

    #[jsg_method]
    fn add<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        input: jsg::Promise<'s, u32>,
        done: jsg::Promise<'s, ()>,
        any: jsg::Promise<'s, v8::Value>,
    ) -> Result<jsg::Promise<'s, v8::Value>, jsg::Error> {
        let _: jsg::Promise<'s, ()> = done.when_resolved();
        let _: jsg::Promise<'s, String> = input.then_or_catch(
            lock,
            String::from("n"),
            |_lock, prefix, n| Ok(format!("{prefix}{n}")),
            |_lock, _prefix, _reason| Ok(String::new()),
        )?;
        any.catch(lock, (), |_lock, (), reason| Ok(reason))
    }

    #[jsg_method]
    fn chain<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        input: jsg::Promise<'s, u32>,
    ) -> Result<Option<jsg::Promise<'s, String>>, jsg::Error> {
        let chained: jsg::Promise<'s, String> = input.then_promise(lock, (), |lock, (), n| {
            jsg::Promise::resolved(lock, n.to_string())
        })?;
        let recovered = chained.catch_promise(lock, (), |lock, (), _reason| {
            jsg::Promise::resolved(lock, String::new())
        })?;
        Ok(Some(recovered))
    }
}
