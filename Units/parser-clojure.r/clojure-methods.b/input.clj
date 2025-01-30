(ns multimethod.test)

(defmulti test
  (fn [type] type))

(defmulti documented-multimethod "Documentation"
  (fn [type] type))

(defmethod test nil
  [& _]
  nil)

(defmethod test :test
  [& _]
  nil)

(defmethod test :test2 named-method
  [& _]
  nil)
