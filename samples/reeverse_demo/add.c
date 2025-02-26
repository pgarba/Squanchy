int add(int a, int b) {
  int arr[] = {1, 2, 3, 4, 5};
  int sum = 0;

  // Loop to calcuate a constant
  for (int i = 0; i < 5; i++) {
    sum += arr[i];
  }

  // MBA based Opaque Predicate
  int A = -1 * b + 1 * ~(a | b) + 1 * (a & b);
  int B = (1 * ~b - 1 * (a ^ b));
  int c = 0;
  if ((A - B) == 0) {
    c = 1911;
  } else {
    c = 23;
  }
  return a + b + c + sum;
}
