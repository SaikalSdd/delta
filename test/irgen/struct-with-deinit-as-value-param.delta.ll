
%"A<int>" = type { i32 }

define void @_ENM4main1AI3intE6deinitE(%"A<int>"* %this) {
  ret void
}

define void @_EN4main3fooE1a1AI3intE(%"A<int>" %a) {
  %1 = alloca %"A<int>"
  store %"A<int>" %a, %"A<int>"* %1
  call void @_ENM4main1AI3intE6deinitE(%"A<int>"* %1)
  ret void
}