use std::hash::Hash;
use std::hash::Hasher;
use std::mem;
use std::ops::Deref;
use std::ops::DerefMut;

use crate::Array;
use crate::ExternFn;
use crate::Future;
use crate::Include;
use crate::Lifetimes;
use crate::Ptr;
use crate::Receiver;
use crate::Ref;
use crate::Signature;
use crate::SliceRef;
use crate::Ty1;
use crate::Type;
use crate::Var;

impl PartialEq for Include {
    fn eq(&self, other: &Self) -> bool {
        let Self {
            cfg: _,
            path,
            kind,
            begin_span: _,
            end_span: _,
        } = self;
        let Self {
            cfg: _,
            path: path2,
            kind: kind2,
            begin_span: _,
            end_span: _,
        } = other;
        path == path2 && kind == kind2
    }
}

impl Deref for ExternFn {
    type Target = Signature;

    fn deref(&self) -> &Self::Target {
        &self.sig
    }
}

impl DerefMut for ExternFn {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.sig
    }
}

impl Hash for Type {
    fn hash<H: Hasher>(&self, state: &mut H) {
        mem::discriminant(self).hash(state);
        match self {
            Self::Ident(t) => t.hash(state),
            Self::RustBox(t) => t.hash(state),
            Self::UniquePtr(t) => t.hash(state),
            Self::KjOwn(t) => t.hash(state),
            Self::KjRc(t) => t.hash(state),
            Self::KjArc(t) => t.hash(state),
            Self::SharedPtr(t) => t.hash(state),
            Self::WeakPtr(t) => t.hash(state),
            Self::Ref(t) => t.hash(state),
            Self::Ptr(t) => t.hash(state),
            Self::KjMaybe(t) => t.hash(state),
            Self::Str(t) => t.hash(state),
            Self::RustVec(t) => t.hash(state),
            Self::CxxVector(t) => t.hash(state),
            Self::Fn(t) => t.hash(state),
            Self::SliceRef(t) => t.hash(state),
            Self::Array(t) => t.hash(state),
            Self::Void(_) | Self::KjDate(_) => {}
            Self::Future(t) => t.hash(state),
        }
    }
}

impl Eq for Type {}

impl PartialEq for Type {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::Ident(lhs), Self::Ident(rhs)) => lhs == rhs,
            (Self::RustBox(lhs), Self::RustBox(rhs)) => lhs == rhs,
            (Self::UniquePtr(lhs), Self::UniquePtr(rhs)) => lhs == rhs,
            (Self::KjOwn(lhs), Self::KjOwn(rhs)) => lhs == rhs,
            (Self::SharedPtr(lhs), Self::SharedPtr(rhs)) => lhs == rhs,
            (Self::WeakPtr(lhs), Self::WeakPtr(rhs)) => lhs == rhs,
            (Self::Ref(lhs), Self::Ref(rhs)) => lhs == rhs,
            (Self::Str(lhs), Self::Str(rhs)) => lhs == rhs,
            (Self::RustVec(lhs), Self::RustVec(rhs)) => lhs == rhs,
            (Self::CxxVector(lhs), Self::CxxVector(rhs)) => lhs == rhs,
            (Self::Fn(lhs), Self::Fn(rhs)) => lhs == rhs,
            (Self::SliceRef(lhs), Self::SliceRef(rhs)) => lhs == rhs,
            (Self::Void(_), Self::Void(_)) => true,
            (Self::KjDate(_), Self::KjDate(_)) => true,
            (Self::Future(lhs), Self::Future(rhs)) => lhs == rhs,
            (_, _) => false,
        }
    }
}

impl Eq for Lifetimes {}

impl PartialEq for Lifetimes {
    fn eq(&self, other: &Self) -> bool {
        let Self {
            lt_token: _,
            lifetimes,
            gt_token: _,
        } = self;
        let Self {
            lt_token: _,
            lifetimes: lifetimes2,
            gt_token: _,
        } = other;
        lifetimes.iter().eq(lifetimes2)
    }
}

impl Hash for Lifetimes {
    fn hash<H: Hasher>(&self, state: &mut H) {
        let Self {
            lt_token: _,
            lifetimes,
            gt_token: _,
        } = self;
        lifetimes.len().hash(state);
        for lifetime in lifetimes {
            lifetime.hash(state);
        }
    }
}

impl Eq for Ty1 {}

impl PartialEq for Ty1 {
    fn eq(&self, other: &Self) -> bool {
        let Self {
            name,
            langle: _,
            inner,
            rangle: _,
        } = self;
        let Self {
            name: name2,
            langle: _,
            inner: inner2,
            rangle: _,
        } = other;
        name == name2 && inner == inner2
    }
}

impl Hash for Ty1 {
    fn hash<H: Hasher>(&self, state: &mut H) {
        let Self {
            name,
            langle: _,
            inner,
            rangle: _,
        } = self;
        name.hash(state);
        inner.hash(state);
    }
}

impl Eq for Ref {}

impl PartialEq for Ref {
    fn eq(&self, other: &Self) -> bool {
        let Self {
            pinned,
            ampersand: _,
            lifetime,
            mutable,
            inner,
            pin_tokens: _,
            mutability: _,
        } = self;
        let Self {
            pinned: pinned2,
            ampersand: _,
            lifetime: lifetime2,
            mutable: mutable2,
            inner: inner2,
            pin_tokens: _,
            mutability: _,
        } = other;
        pinned == pinned2 && lifetime == lifetime2 && mutable == mutable2 && inner == inner2
    }
}

impl Hash for Ref {
    fn hash<H: Hasher>(&self, state: &mut H) {
        let Self {
            pinned,
            ampersand: _,
            lifetime,
            mutable,
            inner,
            pin_tokens: _,
            mutability: _,
        } = self;
        pinned.hash(state);
        lifetime.hash(state);
        mutable.hash(state);
        inner.hash(state);
    }
}

impl Eq for Ptr {}

impl PartialEq for Ptr {
    fn eq(&self, other: &Self) -> bool {
        let Self {
            star: _,
            mutable,
            inner,
            mutability: _,
            constness: _,
        } = self;
        let Self {
            star: _,
            mutable: mutable2,
            inner: inner2,
            mutability: _,
            constness: _,
        } = other;
        mutable == mutable2 && inner == inner2
    }
}

impl Hash for Ptr {
    fn hash<H: Hasher>(&self, state: &mut H) {
        let Self {
            star: _,
            mutable,
            inner,
            mutability: _,
            constness: _,
        } = self;
        mutable.hash(state);
        inner.hash(state);
    }
}

impl Eq for SliceRef {}

impl PartialEq for SliceRef {
    fn eq(&self, other: &Self) -> bool {
        let Self {
            ampersand: _,
            lifetime,
            mutable,
            bracket: _,
            inner,
            mutability: _,
        } = self;
        let Self {
            ampersand: _,
            lifetime: lifetime2,
            mutable: mutable2,
            bracket: _,
            inner: inner2,
            mutability: _,
        } = other;
        lifetime == lifetime2 && mutable == mutable2 && inner == inner2
    }
}

impl Hash for SliceRef {
    fn hash<H: Hasher>(&self, state: &mut H) {
        let Self {
            ampersand: _,
            lifetime,
            mutable,
            bracket: _,
            inner,
            mutability: _,
        } = self;
        lifetime.hash(state);
        mutable.hash(state);
        inner.hash(state);
    }
}

impl Eq for Array {}

impl PartialEq for Array {
    fn eq(&self, other: &Self) -> bool {
        let Self {
            bracket: _,
            inner,
            semi_token: _,
            len,
            len_token: _,
        } = self;
        let Self {
            bracket: _,
            inner: inner2,
            semi_token: _,
            len: len2,
            len_token: _,
        } = other;
        inner == inner2 && len == len2
    }
}

impl PartialEq for Future {
    fn eq(&self, other: &Self) -> bool {
        self.output == other.output
    }
}

impl Hash for Array {
    fn hash<H: Hasher>(&self, state: &mut H) {
        let Self {
            bracket: _,
            inner,
            semi_token: _,
            len,
            len_token: _,
        } = self;
        inner.hash(state);
        len.hash(state);
    }
}

impl Hash for Future {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.output.hash(state);
    }
}

impl Eq for Signature {}

impl PartialEq for Signature {
    fn eq(&self, other: &Self) -> bool {
        let Self {
            asyncness,
            unsafety,
            fn_token: _,
            generics: _,
            receiver,
            args,
            ret,
            throws,
            paren_token: _,
            throws_tokens: _,
        } = self;
        let Self {
            asyncness: asyncness2,
            unsafety: unsafety2,
            fn_token: _,
            generics: _,
            receiver: receiver2,
            args: args2,
            ret: ret2,
            throws: throws2,
            paren_token: _,
            throws_tokens: _,
        } = other;
        asyncness.is_some() == asyncness2.is_some()
            && unsafety.is_some() == unsafety2.is_some()
            && receiver == receiver2
            && ret == ret2
            && throws == throws2
            && args.len() == args2.len()
            && args.iter().zip(args2).all(|(arg, arg2)| {
                let Var {
                    cfg: _,
                    doc: _,
                    attrs: _,
                    visibility: _,
                    name: _,
                    colon_token: _,
                    ty,
                } = arg;
                let Var {
                    cfg: _,
                    doc: _,
                    attrs: _,
                    visibility: _,
                    name: _,
                    colon_token: _,
                    ty: ty2,
                } = arg2;
                ty == ty2
            })
    }
}

impl Hash for Signature {
    fn hash<H: Hasher>(&self, state: &mut H) {
        let Self {
            asyncness,
            unsafety,
            fn_token: _,
            generics: _,
            receiver,
            args,
            ret,
            throws,
            paren_token: _,
            throws_tokens: _,
        } = self;
        asyncness.is_some().hash(state);
        unsafety.is_some().hash(state);
        receiver.hash(state);
        for arg in args {
            let Var {
                cfg: _,
                doc: _,
                attrs: _,
                visibility: _,
                name: _,
                colon_token: _,
                ty,
            } = arg;
            ty.hash(state);
        }
        ret.hash(state);
        throws.hash(state);
    }
}

impl Eq for Receiver {}

impl PartialEq for Receiver {
    fn eq(&self, other: &Self) -> bool {
        let Self {
            pinned,
            ampersand: _,
            lifetime,
            mutable,
            var: _,
            colon_token: _,
            ty,
            shorthand: _,
            pin_tokens: _,
            mutability: _,
        } = self;
        let Self {
            pinned: pinned2,
            ampersand: _,
            lifetime: lifetime2,
            mutable: mutable2,
            var: _,
            colon_token: _,
            ty: ty2,
            shorthand: _,
            pin_tokens: _,
            mutability: _,
        } = other;
        pinned == pinned2 && lifetime == lifetime2 && mutable == mutable2 && ty == ty2
    }
}

impl Hash for Receiver {
    fn hash<H: Hasher>(&self, state: &mut H) {
        let Self {
            pinned,
            ampersand: _,
            lifetime,
            mutable,
            var: _,
            colon_token: _,
            ty,
            shorthand: _,
            pin_tokens: _,
            mutability: _,
        } = self;
        pinned.hash(state);
        lifetime.hash(state);
        mutable.hash(state);
        ty.hash(state);
    }
}
