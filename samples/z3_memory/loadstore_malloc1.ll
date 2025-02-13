define dso_local i32 @square(i32 noundef %0) #0 {
  %2 = alloca i32, align 4
  %3 = alloca ptr, align 8
  %4 = alloca ptr, align 8
  store i32 %0, ptr %2, align 4
  %5 = call noalias ptr @malloc(i64 noundef 100) #2
  store ptr %5, ptr %3, align 8
  %6 = load ptr, ptr %3, align 8
  %7 = getelementptr inbounds i8, ptr %6, i64 0
  store i8 1, ptr %7, align 1
  %8 = load ptr, ptr %3, align 8
  %9 = getelementptr inbounds i8, ptr %8, i64 1
  store i8 2, ptr %9, align 1
  %10 = load ptr, ptr %3, align 8
  %11 = getelementptr inbounds i8, ptr %10, i64 2
  store i8 3, ptr %11, align 1
  %12 = load ptr, ptr %3, align 8
  %13 = getelementptr inbounds i8, ptr %12, i64 3
  store i8 4, ptr %13, align 1
  %14 = load ptr, ptr %3, align 8
  store ptr %14, ptr %4, align 8
  %15 = load ptr, ptr %4, align 8
  %16 = getelementptr inbounds i32, ptr %15, i64 0
  store i32 1432778632, ptr %16, align 4
  %17 = load ptr, ptr %4, align 8
  %18 = getelementptr inbounds i32, ptr %17, i64 0
  %19 = load i32, ptr %18, align 4
  ret i32 %19
}

declare noalias ptr @malloc(i64 noundef) #1

