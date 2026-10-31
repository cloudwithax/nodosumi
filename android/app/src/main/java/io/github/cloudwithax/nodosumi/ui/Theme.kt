package io.github.cloudwithax.nodosumi.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.sp
import io.github.cloudwithax.nodosumi.R

// sumi (ink), washi (paper), shu (the ensō's vermilion), ai (indigo)
val Shu = Color(0xFFE8470A)
val Ink = Color(0xFF141110)
val Washi = Color(0xFFF6F1E7)
private val Ai = Color(0xFF3B5A8C)

private val Dark = darkColorScheme(
    primary = Color(0xFFFF6A2E),
    onPrimary = Color(0xFF2B0B00),
    primaryContainer = Color(0xFF6B2304),
    onPrimaryContainer = Color(0xFFFFDBCC),
    secondary = Color(0xFFD8C2B4),
    onSecondary = Color(0xFF3B2D24),
    secondaryContainer = Color(0xFF3A302B),
    onSecondaryContainer = Color(0xFFF5DED0),
    tertiary = Color(0xFFAAC7FF),
    onTertiary = Color(0xFF0A2F5F),
    background = Ink,
    onBackground = Color(0xFFEDE5DB),
    surface = Ink,
    onSurface = Color(0xFFEDE5DB),
    surfaceVariant = Color(0xFF2A2421),
    onSurfaceVariant = Color(0xFFCDBFB5),
    surfaceContainerLowest = Color(0xFF0E0C0B),
    surfaceContainerLow = Color(0xFF1A1715),
    surfaceContainer = Color(0xFF1F1B19),
    surfaceContainerHigh = Color(0xFF2A2522),
    surfaceContainerHighest = Color(0xFF35302C),
    outline = Color(0xFF978C84),
    outlineVariant = Color(0xFF4A413B),
)

private val Light = lightColorScheme(
    primary = Shu,
    onPrimary = Color.White,
    primaryContainer = Color(0xFFFFDBCC),
    onPrimaryContainer = Color(0xFF390C00),
    secondary = Color(0xFF6E5A4D),
    onSecondary = Color.White,
    secondaryContainer = Color(0xFFF2DED0),
    onSecondaryContainer = Color(0xFF26190F),
    tertiary = Ai,
    onTertiary = Color.White,
    background = Washi,
    onBackground = Color(0xFF1F1A17),
    surface = Washi,
    onSurface = Color(0xFF1F1A17),
    surfaceVariant = Color(0xFFEDE3D6),
    onSurfaceVariant = Color(0xFF51453D),
    surfaceContainerLowest = Color.White,
    surfaceContainerLow = Color(0xFFFBF6EE),
    surfaceContainer = Color(0xFFF1EADF),
    surfaceContainerHigh = Color(0xFFEBE3D7),
    surfaceContainerHighest = Color(0xFFE5DCCF),
    outline = Color(0xFF84756B),
    outlineVariant = Color(0xFFD7C9BD),
)

// oswald for headings and the brand, open sans for everything else, like the desktop ui
val Oswald = FontFamily(Font(R.font.oswald_medium))
private val OpenSans = FontFamily(Font(R.font.opensans_regular))

private val Type = Typography().run {
    copy(
        displayLarge = displayLarge.copy(fontFamily = Oswald),
        displayMedium = displayMedium.copy(fontFamily = Oswald),
        displaySmall = displaySmall.copy(fontFamily = Oswald),
        headlineLarge = headlineLarge.copy(fontFamily = Oswald),
        headlineMedium = headlineMedium.copy(fontFamily = Oswald),
        headlineSmall = headlineSmall.copy(fontFamily = Oswald),
        titleLarge = titleLarge.copy(fontFamily = Oswald, fontSize = 24.sp),
        titleMedium = titleMedium.copy(fontFamily = OpenSans),
        titleSmall = titleSmall.copy(fontFamily = OpenSans),
        bodyLarge = bodyLarge.copy(fontFamily = OpenSans),
        bodyMedium = bodyMedium.copy(fontFamily = OpenSans),
        bodySmall = bodySmall.copy(fontFamily = OpenSans),
        labelLarge = labelLarge.copy(fontFamily = OpenSans),
        labelMedium = labelMedium.copy(fontFamily = OpenSans),
        labelSmall = labelSmall.copy(fontFamily = OpenSans),
    )
}

/** Section headers, small caps in the accent like azahar's settings lists. */
val SectionStyle = TextStyle(fontFamily = Oswald, fontSize = 14.sp, letterSpacing = 1.2.sp)

@Composable
fun NodosumiTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = if (isSystemInDarkTheme()) Dark else Light,
        typography = Type,
        content = content,
    )
}
