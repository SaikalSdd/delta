
%S = type { i32 }

define void @_ENM4main1S3fooE(%S* %this) {
  %b = alloca i32
  %a = getelementptr inbounds %S, %S* %this, i32 0, i32 0
  %a1 = load i32, i32* %a
  store i32 %a1, i32* %b
  ret void
}