define dso_local zeroext range(i16 1, 3) i16 @add(i16 noundef zeroext %0, i16 noundef zeroext %1) local_unnamed_addr #0 {
  %3 = and i16 %1, %0
  %4 = shl i16 %3, 2
  %5 = xor i16 %1, -1
  %6 = and i16 %0, %5
  %7 = mul i16 %6, 3
  %8 = shl i16 %0, 1
  %9 = or i16 %0, %5
  %10 = or i16 %1, %0
  %11 = add i16 %10, 1
  %12 = xor i16 %9, -1
  %13 = xor i16 %3, -1
  %14 = add i16 %4, %13
  %15 = add i16 %14, %7
  %16 = add i16 %11, %9
  %17 = mul i16 %16, %8
  %18 = sub i16 %12, %10
  %19 = mul i16 %18, 5
  %20 = add i16 %15, %19
  %21 = mul i16 %20, %0
  %22 = add i16 %21, %17
  %23 = sub i16 0, %0
  %24 = icmp eq i16 %22, %23
  %25 = select i1 %24, i16 1, i16 2
  ret i16 %25
}
