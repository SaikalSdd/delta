
%string = type { %"ArrayRef<char>" }
%"ArrayRef<char>" = type { i8*, i32 }
%StringBuffer = type { %"List<char>" }
%"List<char>" = type { i8*, i32, i32 }

@0 = private unnamed_addr constant [4 x i8] c"foo\00", align 1
@1 = private unnamed_addr constant [6 x i8] c"%.*s\0A\00", align 1

define i32 @main() {
  %__str = alloca %string
  call void @_EN3std6string4initEP4char3int(%string* %__str, i8* getelementptr inbounds ([4 x i8], [4 x i8]* @0, i32 0, i32 0), i32 3)
  call void @_EN3std7printlnI6stringEEP6string(%string* %__str)
  ret i32 0
}

define void @_EN3std7printlnI6stringEEP6string(%string* %value) {
  %s = alloca %StringBuffer
  %1 = call %StringBuffer @_EN3std6string8toStringE(%string* %value)
  store %StringBuffer %1, %StringBuffer* %s
  %2 = call i32 @_EN3std12StringBuffer4sizeE(%StringBuffer* %s)
  %3 = call i8* @_EN3std12StringBuffer4dataE(%StringBuffer* %s)
  %4 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([6 x i8], [6 x i8]* @1, i32 0, i32 0), i32 %2, i8* %3)
  call void @_EN3std12StringBuffer6deinitE(%StringBuffer* %s)
  ret void
}

declare void @_EN3std6string4initEP4char3int(%string*, i8*, i32)

declare void @_EN3std12StringBuffer6deinitE(%StringBuffer*)

declare %StringBuffer @_EN3std6string8toStringE(%string*)

declare i32 @printf(i8*, ...)

declare i32 @_EN3std12StringBuffer4sizeE(%StringBuffer*)

declare i8* @_EN3std12StringBuffer4dataE(%StringBuffer*)
