(ns vars.test)

(def var 'var)

(def doc-var "Documentation" 'var)

(def dynamic-var ^:dynamic 'var)

(def const-var ^:const 'var)

(defonce once-var 'evaluated-once)

(def fn-var (fn [] true))
