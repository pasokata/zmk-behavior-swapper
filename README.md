# ZMK BEHAVIOR SWAPPER

## Usage
```
#include <behaviors.dtsi>
#include <behaviors/swapper.dtsi>
#include <dt-bindings/zmk/keys.h>
/ {
    keymap {
        compatible = "zmk,keymap";

        default_layer {
            bindings = <
                &sw LALT TAB        &kp ESC
                &sw LALT LS(TAB)    &kp ESC
            >;
        };
    };
};

```