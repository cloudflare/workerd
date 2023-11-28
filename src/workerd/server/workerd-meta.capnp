@0xfec9bacd5b113388;
# We embed the original text of the config schema so that it doesn't have to be accessible on the
# filesystem when parsing a workerd config file.

$import "/capnp/c++.capnp".namespace("workerd::server");

const cppCapnpSchema :Text = embed "/capnp/c++.capnp";
const workerdCapnpSchema :Text = embed "workerd.capnp";
const autogateCapnpSchema :Text = embed "../util/autogate.capnp";
